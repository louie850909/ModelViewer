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
};

struct Mesh {
    std::vector<Vertex>   vertices;
    std::vector<uint32_t> indices;
    std::vector<SubMesh>  subMeshes;
    // 記錄解析出來的貼圖絕對路徑 (索引對應 materialIndex)
    std::vector<std::string> texturePaths;

    // GPU 資源
    ComPtr<ID3D12Resource> vertexBuffer;
    ComPtr<ID3D12Resource> indexBuffer;
    D3D12_VERTEX_BUFFER_VIEW vbView = {};
    D3D12_INDEX_BUFFER_VIEW  ibView = {};
};