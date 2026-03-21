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

    for (unsigned int m = 0; m < scene->mNumMeshes; m++) {
        aiMesh* aiM = scene->mMeshes[m];
        SubMesh sub;
        sub.indexOffset = (UINT)mesh->indices.size();
        sub.indexCount = aiM->mNumFaces * 3;

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
    return mesh;
}