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
    UINT indexOffset;
    UINT indexCount;
    int  materialIndex = -1;
    bool isTransparent = false;
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