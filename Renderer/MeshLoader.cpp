#include "pch.h"
#include "MeshLoader.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

// tinygltf 用到 stb，stb 用了 sprintf，需要壓制 MSVC 的安全警告
#pragma warning(push)
#pragma warning(disable: 4996)
#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <tiny_gltf.h>
#pragma warning(pop)

#include <filesystem>

namespace {
    // 遞迴解析 Assimp 節點
    void ParseAssimpNode(aiNode* node, int parentIndex, std::vector<SceneNode>& outNodes) {
        int currentIndex = (int)outNodes.size();
        SceneNode sn;
        sn.name = node->mName.C_Str();
        if (sn.name.empty()) sn.name = "Unnamed_Node";
        sn.parentIndex = parentIndex;

        // 分解 Assimp 轉換矩陣
        aiVector3D scaling, position;
        aiQuaternion rotation;
        node->mTransformation.Decompose(scaling, rotation, position);

        sn.t[0] = position.x; sn.t[1] = position.y; sn.t[2] = position.z;
        sn.r[0] = rotation.x; sn.r[1] = rotation.y; sn.r[2] = rotation.z; sn.r[3] = rotation.w;
        sn.s[0] = scaling.x; sn.s[1] = scaling.y; sn.s[2] = scaling.z;

        outNodes.push_back(sn);

        for (unsigned int i = 0; i < node->mNumChildren; i++) {
            ParseAssimpNode(node->mChildren[i], currentIndex, outNodes);
        }
    }

    // 遞迴解析 glTF 節點
    void ParseGltfNode(const tinygltf::Model& model, int nodeIndex, int parentIndex, std::vector<SceneNode>& outNodes) {
        int currentIndex = (int)outNodes.size();
        SceneNode sn;
        const auto& gltfNode = model.nodes[nodeIndex];
        sn.name = gltfNode.name;
        if (sn.name.empty()) sn.name = "Node_" + std::to_string(nodeIndex);
        sn.parentIndex = parentIndex;

        // glTF 可能使用 TRS 陣列 (主動cast以迴避編譯警告)
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

        outNodes.push_back(sn);

        for (int childIndex : gltfNode.children) {
            ParseGltfNode(model, childIndex, currentIndex, outNodes);
        }
    }
}

std::shared_ptr<Mesh> MeshLoader::Load(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    // 統一小寫比較
    for (auto& c : ext) c = (char)tolower(c);

    if (ext == ".gltf" || ext == ".glb")
        return LoadGltf(path);
    else
        return LoadViaAssimp(path); // .fbx, .obj, .vrm (vrm = glb 實際上)
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

    // 遍歷 Assimp 的材質陣列
    for (unsigned int i = 0; i < scene->mNumMaterials; i++) {
        aiMaterial* mat = scene->mMaterials[i];
        aiString texPath;
        // 嘗試取得 Diffuse (BaseColor) 貼圖
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
        //記錄這個 aiMesh 使用哪個材質
        sub.materialIndex = aiM->mMaterialIndex;

        // --------- Assimp 的半透明判斷邏輯 ---------
        if (aiM->mMaterialIndex >= 0) {
            aiMaterial* mat = scene->mMaterials[aiM->mMaterialIndex];

            // 條件 1：檢查整體的「不透明度 (Opacity)」數值 (1.0 = 完全不透明)
            float opacity = 1.0f;
            mat->Get(AI_MATKEY_OPACITY, opacity);

            // 條件 2：檢查是否有綁定「透明度貼圖 (Opacity Map)」
            bool hasOpacityTexture = (mat->GetTextureCount(aiTextureType_OPACITY) > 0);

            // 條件 3：某些舊版 OBJ 格式會把透明貼圖當作 DISPLACEMENT 或 SHININESS 傳入，
            // 但最正規的做法是檢查 Diffuse 貼圖是否本身就被標記為帶有 Alpha
            // (為了保險起見，只要滿足條件 1 或條件 2，我們就視為半透明)

            sub.isTransparent = (opacity < 1.0f) || hasOpacityTexture;
        }
        else {
            sub.isTransparent = false;
        }
        // --------------------------------------------------

        // 紀錄這個 aiMesh 在全局頂點陣列中的起始位置
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
            // 寫入 Index 時，必須加上 vertexOffset！
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

    // 取得模型所在的資料夾路徑 (用來組合貼圖絕對路徑)
    std::string baseDir = std::filesystem::path(path).parent_path().string() + "/";

    // 遍歷 glTF 的材質陣列
    for (const auto& mat : model.materials) {
        std::string texPath = "";
        std::string mrPath = "";
        // 檢查是否有 BaseColor 貼圖
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
        mesh->texturePaths.push_back(texPath); // 就算沒有貼圖也塞入空字串佔位
        mesh->metallicRoughnessPaths.push_back(mrPath);
    }

    // 在迴圈外定義一個輔助函式，用來安全取得 Byte Stride
    auto GetStride = [](const tinygltf::Accessor& acc, const tinygltf::BufferView& view) -> size_t {
        if (view.byteStride != 0) return view.byteStride;
        return tinygltf::GetComponentSizeInBytes(acc.componentType) * tinygltf::GetNumComponentsInType(acc.type);
        };

    for (auto& gMesh : model.meshes) {
        for (const auto& prim : gMesh.primitives) {

            // 記錄這個子網格在「全局 Vertex Buffer」中的起始位置
            UINT vertexOffset = (UINT)mesh->vertices.size();

            // 取得 Position 資源
            auto& posAcc = model.accessors[prim.attributes.at("POSITION")];
            auto& posView = model.bufferViews[posAcc.bufferView];
            size_t posStride = GetStride(posAcc, posView);
            const uint8_t* posBase = model.buffers[posView.buffer].data.data() + posView.byteOffset + posAcc.byteOffset;

            // 取得 Normal 資源 (如果有)
            const uint8_t* nrmBase = nullptr;
            size_t nrmStride = 0;
            if (prim.attributes.count("NORMAL")) {
                auto& nrmAcc = model.accessors[prim.attributes.at("NORMAL")];
                auto& nrmView = model.bufferViews[nrmAcc.bufferView];
                nrmStride = GetStride(nrmAcc, nrmView);
                nrmBase = model.buffers[nrmView.buffer].data.data() + nrmView.byteOffset + nrmAcc.byteOffset;
            }

            // 取得 UV 資源 (如果有)
            const uint8_t* uvBase = nullptr;
            size_t uvStride = 0;
            if (prim.attributes.count("TEXCOORD_0")) {
                auto& uvAcc = model.accessors[prim.attributes.at("TEXCOORD_0")];
                auto& uvView = model.bufferViews[uvAcc.bufferView];
                uvStride = GetStride(uvAcc, uvView);
                uvBase = model.buffers[uvView.buffer].data.data() + uvView.byteOffset + uvAcc.byteOffset;
            }

            // 使用 Pointer Math 加上 Stride 來精準跳躍記憶體
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

            // 處理 Indices 時，確保每個 Index 都加上 vertexOffset
            SubMesh sub;
            sub.indexOffset = (UINT)mesh->indices.size(); // 記錄這個子網格的起始 Index

            // 記錄這個子網格使用哪個材質
            sub.materialIndex = prim.material;
			sub.isTransparent = (prim.material >= 0 && model.materials[prim.material].alphaMode == "BLEND");

            auto& idxAcc = model.accessors[prim.indices];
            auto& idxView = model.bufferViews[idxAcc.bufferView];
            const uint8_t* idxRaw = model.buffers[idxView.buffer].data.data() + idxView.byteOffset + idxAcc.byteOffset;

            sub.indexCount = (UINT)idxAcc.count;

            for (size_t i = 0; i < idxAcc.count; i++) {
                if (idxAcc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
                    const uint16_t* buf = (const uint16_t*)idxRaw;
                    mesh->indices.push_back(buf[i] + vertexOffset); // 補上偏移
                }
                else if (idxAcc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT) {
                    const uint32_t* buf = (const uint32_t*)idxRaw;
                    mesh->indices.push_back(buf[i] + vertexOffset); // 補上偏移
                }
            }
            mesh->subMeshes.push_back(sub);
        }
    }

    int defaultScene = model.defaultScene > -1 ? model.defaultScene : 0;
    if (defaultScene < model.scenes.size()) {
        for (int nodeIdx : model.scenes[defaultScene].nodes) {
            ParseGltfNode(model, nodeIdx, -1, mesh->nodes);
        }
    }
    return mesh;
}