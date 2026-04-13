#pragma once
#include "pch.h"
#include <vector>
#include <string>

struct Vertex {
    DirectX::XMFLOAT3 position;
    DirectX::XMFLOAT3 normal;
    DirectX::XMFLOAT2 uv;
};

struct SubMesh {
	UINT indexOffset = 0; // 在全局 Index Buffer 中的起始位置 (單位是 Index，不是 Byte)
	UINT indexCount = 0;
    int  materialIndex = -1;
    bool isTransparent = false; // 用於玻璃、水滴 (Alpha Blend / Transmission)
    bool isAlphaTested = false; // 用於樹葉、鐵絲網 (Alpha Mask)

    // KHR_materials_transmission
    float transmissionFactor = 0.0f;  // 0=不透明, 1=全穿透
    float ior = 1.5f;                 // 折射率，預設玻璃

    // 新增：baseColorFactor (無貼圖時的基底色)
    float baseColorFactor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    bool  hasBaseColorTexture = false;
};

struct SceneNode {
    std::string name;
    int parentIndex = -1; // -1 代表這是根節點 (沒有父節點)
    float t[3] = { 0.0f, 0.0f, 0.0f };          // Translation
    float r[4] = { 0.0f, 0.0f, 0.0f, 1.0f };    // Rotation (Quaternion: x, y, z, w)
    float s[3] = { 1.0f, 1.0f, 1.0f };          // Scale

    // 記錄這個節點要畫哪些子網格 (儲存 m_mesh->subMeshes 的 Index)
    std::vector<int> subMeshIndices;
};

struct Mesh {
    std::vector<Vertex>   vertices;
    std::vector<uint32_t> indices;
    std::vector<SubMesh>  subMeshes;
    // 記錄解析出來的貼圖絕對路徑 (索引對應 materialIndex)
    std::vector<std::string> texturePaths;
    std::vector<std::string> metallicRoughnessPaths;
    std::vector<std::string> normalPaths;

    // 用來儲存整個場景的節點層級 (攤平的陣列)
    std::vector<SceneNode> nodes;

    // GPU 資源
    ComPtr<ID3D12Resource> vertexBuffer;
    ComPtr<ID3D12Resource> indexBuffer;
    D3D12_VERTEX_BUFFER_VIEW vbView = {};
    D3D12_INDEX_BUFFER_VIEW  ibView = {};

    // --- DXR BLAS 資源 ---
    std::vector<ComPtr<ID3D12Resource>> blasBuffers;
};

struct alignas(4) MaterialConstants {
    uint32_t textureIndex;
    float    transmissionFactor;
    float    ior;
    float    baseColorFactor[4];
    uint32_t _pad;  // 對齊至 32 bytes
};