#include "pch.h"
#include "MeshLoader.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

// tinygltf は stb を使用し、stb は sprintf を使用するため、MSVC のセキュリティ警告を抑制する必要がある
#pragma warning(push)
#pragma warning(disable: 4996)
#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <tiny_gltf.h>
#pragma warning(pop)

#include <filesystem>

namespace {
    // Assimp ノードを再帰的に解析
    void ParseAssimpNode(aiNode* node, int parentIndex, std::vector<SceneNode>& outNodes) {
        int currentIndex = (int)outNodes.size();
        SceneNode sn;
        sn.name = node->mName.C_Str();
        if (sn.name.empty()) sn.name = "Unnamed_Node";
        sn.parentIndex = parentIndex;

        // Assimp の変換行列を分解
        aiVector3D scaling, position;
        aiQuaternion rotation;
        node->mTransformation.Decompose(scaling, rotation, position);

        sn.t[0] = position.x; sn.t[1] = position.y; sn.t[2] = position.z;
        sn.r[0] = rotation.x; sn.r[1] = rotation.y; sn.r[2] = rotation.z; sn.r[3] = rotation.w;
        sn.s[0] = scaling.x; sn.s[1] = scaling.y; sn.s[2] = scaling.z;

        // Assimp ノードが持つ Mesh Index を記録
        for (unsigned int i = 0; i < node->mNumMeshes; i++) {
            sn.subMeshIndices.push_back(node->mMeshes[i]);
        }

        outNodes.push_back(sn);

        for (unsigned int i = 0; i < node->mNumChildren; i++) {
            ParseAssimpNode(node->mChildren[i], currentIndex, outNodes);
        }
    }

    // glTF ノードを再帰的に解析
    void ParseGltfNode(const tinygltf::Model& model, int nodeIndex, int parentIndex, std::vector<SceneNode>& outNodes, const std::vector<std::vector<int>>& gltfMeshToSubMeshes) {
        int currentIndex = (int)outNodes.size();
        SceneNode sn;
        const auto& gltfNode = model.nodes[nodeIndex];
        sn.name = gltfNode.name;
        if (sn.name.empty()) sn.name = "Node_" + std::to_string(nodeIndex);
        sn.parentIndex = parentIndex;

        // glTF は TRS 配列を使用する可能性がある (コンパイル警告を回避するため積極的にキャスト)
        if (gltfNode.translation.size() == 3) {
            sn.t[0] = static_cast<float>(gltfNode.translation[0]);
            sn.t[1] = static_cast<float>(gltfNode.translation[1]);
            sn.t[2] = static_cast<float>(gltfNode.translation[2]);
        }
        if (gltfNode.rotation.size() == 4) {
            sn.r[0] = static_cast<float>(gltfNode.rotation[0]);
            sn.r[1] = static_cast<float>(gltfNode.rotation[1]);
            sn.r[2] = static_cast<float>(gltfNode.rotation[2]);
            sn.r[3] = static_cast<float>(gltfNode.rotation[3]);
        }
        if (gltfNode.scale.size() == 3) {
            sn.s[0] = static_cast<float>(gltfNode.scale[0]);
            sn.s[1] = static_cast<float>(gltfNode.scale[1]);
            sn.s[2] = static_cast<float>(gltfNode.scale[2]);
        }

        // このノードにモデルがバインドされている場合、対応する SubMesh を追加
        if (gltfNode.mesh >= 0 && gltfNode.mesh < (int)gltfMeshToSubMeshes.size()) {
            for (int subIdx : gltfMeshToSubMeshes[gltfNode.mesh]) {
                sn.subMeshIndices.push_back(subIdx);
            }
        }

        outNodes.push_back(sn);

        for (int childIndex : gltfNode.children) {
            ParseGltfNode(model, childIndex, currentIndex, outNodes, gltfMeshToSubMeshes);
        }
    }
}

std::shared_ptr<Mesh> MeshLoader::Load(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    // 小文字に統一して比較
    for (auto& c : ext) c = (char)tolower(c);

    if (ext == ".gltf" || ext == ".glb")
        return LoadGltf(path);
    else
        return LoadViaAssimp(path); // .fbx, .obj, .vrm (vrm は実際には glb)
}

std::shared_ptr<Mesh> MeshLoader::LoadViaAssimp(const std::string& path) {
    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(path,
        aiProcess_Triangulate |
        aiProcess_GenNormals |
        aiProcess_FlipUVs |
        aiProcess_JoinIdenticalVertices);

    if (!scene || !scene->HasMeshes()) return nullptr;

    auto mesh = std::make_shared<Mesh>();
    std::string baseDir = std::filesystem::path(path).parent_path().string() + "/";

    // Assimp の材質配列を走査
    for (unsigned int i = 0; i < scene->mNumMaterials; i++) {
        aiMaterial* mat = scene->mMaterials[i];
        aiString texPath;
        // Diffuse (BaseColor) テクスチャの取得を試みる
        if (mat->GetTexture(aiTextureType_DIFFUSE, 0, &texPath) == AI_SUCCESS) {
            mesh->texturePaths.push_back(baseDir + texPath.C_Str());
        }
        else {
            mesh->texturePaths.push_back("");
        }
    }

    for (unsigned int m = 0; m < scene->mNumMeshes; m++) {
        aiMesh* aiM = scene->mMeshes[m];
        SubMesh sub;
        sub.indexOffset = (UINT)mesh->indices.size();
        sub.indexCount = aiM->mNumFaces * 3;
        // この aiMesh が使用する材質を記録
        sub.materialIndex = aiM->mMaterialIndex;

        // --------- Assimp の半透明判定ロジック ---------
        if (aiM->mMaterialIndex >= 0) {
            aiMaterial* mat = scene->mMaterials[aiM->mMaterialIndex];

            // 条件 1：全体の「不透明度 (Opacity)」値を確認 (1.0 = 完全不透明)
            float opacity = 1.0f;
            mat->Get(AI_MATKEY_OPACITY, opacity);

            // 条件 2：「透明度テクスチャ (Opacity Map)」がバインドされているか確認
            bool hasOpacityTexture = (mat->GetTextureCount(aiTextureType_OPACITY) > 0);

            sub.isTransparent = (opacity < 1.0f) || hasOpacityTexture;
        }
        else {
            sub.isTransparent = false;
        }
        // --------------------------------------------------

        // この aiMesh のグローバル頂点配列内での開始位置を記録
        UINT vertexOffset = (UINT)mesh->vertices.size();

        for (unsigned int i = 0; i < aiM->mNumVertices; i++) {
            Vertex v;
            v.position = { aiM->mVertices[i].x, aiM->mVertices[i].y, aiM->mVertices[i].z };
            v.normal = aiM->HasNormals()
                ? DirectX::XMFLOAT3(aiM->mNormals[i].x, aiM->mNormals[i].y, aiM->mNormals[i].z)
                : DirectX::XMFLOAT3(0, 1, 0);
            v.uv = aiM->HasTextureCoords(0)
                ? DirectX::XMFLOAT2(aiM->mTextureCoords[0][i].x, aiM->mTextureCoords[0][i].y)
                : DirectX::XMFLOAT2(0, 0);
            mesh->vertices.push_back(v);
        }

        for (unsigned int i = 0; i < aiM->mNumFaces; i++) {
            // Index を書き込む際は vertexOffset を加算する必要がある！
            mesh->indices.push_back(aiM->mFaces[i].mIndices[0] + vertexOffset);
            mesh->indices.push_back(aiM->mFaces[i].mIndices[1] + vertexOffset);
            mesh->indices.push_back(aiM->mFaces[i].mIndices[2] + vertexOffset);
        }
        mesh->subMeshes.push_back(sub);
    }

    ParseAssimpNode(scene->mRootNode, -1, mesh->nodes);
    return mesh;
}

std::shared_ptr<Mesh> MeshLoader::LoadGltf(const std::string& path) {
    tinygltf::TinyGLTF loader;
    tinygltf::Model    model;
    std::string err, warn;

    bool ok = (path.ends_with(".glb"))
        ? loader.LoadBinaryFromFile(&model, &err, &warn, path)
        : loader.LoadASCIIFromFile(&model, &err, &warn, path);
    if (!ok) return nullptr;

    auto mesh = std::make_shared<Mesh>();

    // モデルが存在するフォルダパスを取得 (テクスチャの絶対パスを組み合わせるため)
    std::string baseDir = std::filesystem::path(path).parent_path().string() + "/";

    // glTF の材質配列を走査
    for (const auto& mat : model.materials) {
        std::string texPath = "";
        std::string mrPath = "";
        std::string normalPath = "";

        // BaseColor テクスチャがあるか確認
        if (mat.pbrMetallicRoughness.baseColorTexture.index >= 0) {
            int texIdx = mat.pbrMetallicRoughness.baseColorTexture.index;
            int imgIdx = model.textures[texIdx].source;
            texPath = baseDir + model.images[imgIdx].uri;
        }

        if (mat.pbrMetallicRoughness.metallicRoughnessTexture.index >= 0) {
            int texIdx = mat.pbrMetallicRoughness.metallicRoughnessTexture.index;
            int imgIdx = model.textures[texIdx].source;
            mrPath = baseDir + model.images[imgIdx].uri;
        }

        if (mat.normalTexture.index >= 0) {
            int texIdx = mat.normalTexture.index;
            int imgIdx = model.textures[texIdx].source;
            normalPath = baseDir + model.images[imgIdx].uri;
        }

        mesh->texturePaths.push_back(texPath); // テクスチャがなくても空文字列でプレースホルダーを追加
        mesh->metallicRoughnessPaths.push_back(mrPath);
        mesh->normalPaths.push_back(normalPath);
    }

    // ループ外でヘルパー関数を定義し、Byte Stride を安全に取得
    auto GetStride = [](const tinygltf::Accessor& acc, const tinygltf::BufferView& view) -> size_t {
        if (view.byteStride != 0) return view.byteStride;
        return tinygltf::GetComponentSizeInBytes(acc.componentType) * tinygltf::GetNumComponentsInType(acc.type);
        };

    // [修正] 誤った外側の for (auto& gMesh) ループを削除し、インデックス m で直接走査するよう変更
    std::vector<std::vector<int>> gltfMeshToSubMeshes(model.meshes.size());
    int currentSubMeshIdx = 0;
    for (size_t m = 0; m < model.meshes.size(); ++m) {
        for (const auto& prim : model.meshes[m].primitives) {

            // このサブメッシュの「グローバル Vertex Buffer」内での開始位置を記録
            UINT vertexOffset = (UINT)mesh->vertices.size();

            // Position リソースを取得
            auto& posAcc = model.accessors[prim.attributes.at("POSITION")];
            auto& posView = model.bufferViews[posAcc.bufferView];
            size_t posStride = GetStride(posAcc, posView);
            const uint8_t* posBase = model.buffers[posView.buffer].data.data() + posView.byteOffset + posAcc.byteOffset;

            // Normal リソースを取得 (存在する場合)
            const uint8_t* nrmBase = nullptr;
            size_t nrmStride = 0;
            if (prim.attributes.count("NORMAL")) {
                auto& nrmAcc = model.accessors[prim.attributes.at("NORMAL")];
                auto& nrmView = model.bufferViews[nrmAcc.bufferView];
                nrmStride = GetStride(nrmAcc, nrmView);
                nrmBase = model.buffers[nrmView.buffer].data.data() + nrmView.byteOffset + nrmAcc.byteOffset;
            }

            // UV リソースを取得 (存在する場合)
            const uint8_t* uvBase = nullptr;
            size_t uvStride = 0;
            if (prim.attributes.count("TEXCOORD_0")) {
                auto& uvAcc = model.accessors[prim.attributes.at("TEXCOORD_0")];
                auto& uvView = model.bufferViews[uvAcc.bufferView];
                uvStride = GetStride(uvAcc, uvView);
                uvBase = model.buffers[uvView.buffer].data.data() + uvView.byteOffset + uvAcc.byteOffset;
            }

            // Stride を加算した Pointer Math を使用してメモリを正確にジャンプ
            for (size_t i = 0; i < posAcc.count; i++) {
                Vertex v;
                const float* p = (const float*)(posBase + i * posStride);
                v.position = { p[0], p[1], p[2] };

                if (nrmBase) {
                    const float* n = (const float*)(nrmBase + i * nrmStride);
                    v.normal = { n[0], n[1], n[2] };
                }
                else {
                    v.normal = { 0, 1, 0 };
                }

                if (uvBase) {
                    const float* u = (const float*)(uvBase + i * uvStride);
                    v.uv = { u[0], u[1] };
                }
                else {
                    v.uv = { 0, 0 };
                }
                mesh->vertices.push_back(v);
            }

            // Indices を処理する際、各 Index に vertexOffset を加算することを確認
            SubMesh sub;
            sub.indexOffset = (UINT)mesh->indices.size(); // このサブメッシュの開始 Index を記録

            // このサブメッシュが使用する材質を記録
            sub.materialIndex = prim.material;
            if (prim.material >= 0 && prim.material < (int)model.materials.size()) {
                const auto& mat = model.materials[prim.material];

                // 葉類 (Mask) かどうかを判定
                sub.isAlphaTested = (mat.alphaMode == "MASK");

                // KHR_materials_transmission を判定
                float transmission = 0.0f;
                auto it = mat.extensions.find("KHR_materials_transmission");
                if (it != mat.extensions.end()) {
                    auto& extVal = it->second;
                    if (extVal.IsObject() && extVal.Has("transmissionFactor")) {
                        transmission = static_cast<float>(
                            extVal.Get("transmissionFactor").GetNumberAsDouble());
                    }
                }
                sub.transmissionFactor = transmission;
                // ガラス/半透明 (Blend または透過) かどうかを判定
                sub.isTransparent = (mat.alphaMode == "BLEND") || (transmission > 0.0f);

                // KHR_materials_ior を解析
                auto iorIt = mat.extensions.find("KHR_materials_ior");
                if (iorIt != mat.extensions.end()) {
                    auto& iorVal = iorIt->second;
                    if (iorVal.IsObject() && iorVal.Has("ior")) {
                        sub.ior = static_cast<float>(iorVal.Get("ior").GetNumberAsDouble());
                    }
                }

                // baseColorFactor を解析
                const auto& bcf = mat.pbrMetallicRoughness.baseColorFactor;
                if (bcf.size() == 4) {
                    sub.baseColorFactor[0] = static_cast<float>(bcf[0]);
                    sub.baseColorFactor[1] = static_cast<float>(bcf[1]);
                    sub.baseColorFactor[2] = static_cast<float>(bcf[2]);
                    sub.baseColorFactor[3] = static_cast<float>(bcf[3]);
                }
                sub.hasBaseColorTexture = (mat.pbrMetallicRoughness.baseColorTexture.index >= 0);
            }
            else {
                sub.isTransparent = false;
            }

            auto& idxAcc = model.accessors[prim.indices];
            auto& idxView = model.bufferViews[idxAcc.bufferView];
            const uint8_t* idxRaw = model.buffers[idxView.buffer].data.data() + idxView.byteOffset + idxAcc.byteOffset;

            sub.indexCount = (UINT)idxAcc.count;

            for (size_t i = 0; i < idxAcc.count; i++) {
                if (idxAcc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
                    const uint16_t* buf = (const uint16_t*)idxRaw;
                    mesh->indices.push_back(buf[i] + vertexOffset); // オフセットを加算
                }
                else if (idxAcc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT) {
                    const uint32_t* buf = (const uint32_t*)idxRaw;
                    mesh->indices.push_back(buf[i] + vertexOffset); // オフセットを加算
                }
            }
            mesh->subMeshes.push_back(sub);

            // この glTF Mesh が生成した SubMesh を記録
            gltfMeshToSubMeshes[m].push_back(currentSubMeshIdx++);
        }
    }

    int defaultScene = model.defaultScene > -1 ? model.defaultScene : 0;
    if (defaultScene < (int)model.scenes.size()) {
        for (int nodeIdx : model.scenes[defaultScene].nodes) {
            ParseGltfNode(model, nodeIdx, -1, mesh->nodes, gltfMeshToSubMeshes);
        }
    }
    return mesh;
}