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
	UINT indexOffset = 0; // グローバル Index Buffer 内の開始位置 (単位は Index、Byte ではない)
	UINT indexCount = 0;
    int  materialIndex = -1;
    bool isTransparent = false; // ガラス、水滴に使用 (Alpha Blend / Transmission)
    bool isAlphaTested = false; // 葉、金網に使用 (Alpha Mask)

    // KHR_materials_transmission
    float transmissionFactor = 0.0f;  // 0=不透明, 1=完全透過
    float ior = 1.5f;                 // 屈折率、デフォルトはガラス

    // 追加：baseColorFactor (テクスチャなし時のベースカラー)
    float baseColorFactor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    bool  hasBaseColorTexture = false;
};

struct SceneNode {
    std::string name;
    int parentIndex = -1; // -1 はルートノード (親ノードなし) を表す
    float t[3] = { 0.0f, 0.0f, 0.0f };          // Translation
    float r[4] = { 0.0f, 0.0f, 0.0f, 1.0f };    // Rotation (Quaternion: x, y, z, w)
    float s[3] = { 1.0f, 1.0f, 1.0f };          // Scale

    // このノードが描画するサブメッシュを記録 (m_mesh->subMeshes の Index を格納)
    std::vector<int> subMeshIndices;
};

struct Mesh {
    std::vector<Vertex>   vertices;
    std::vector<uint32_t> indices;
    std::vector<SubMesh>  subMeshes;
    // 解析されたテクスチャの絶対パスを記録 (インデックスは materialIndex に対応)
    std::vector<std::string> texturePaths;
    std::vector<std::string> metallicRoughnessPaths;
    std::vector<std::string> normalPaths;

    // シーン全体のノード階層を格納 (フラット化された配列)
    std::vector<SceneNode> nodes;

    // GPU リソース
    ComPtr<ID3D12Resource> vertexBuffer;
    ComPtr<ID3D12Resource> indexBuffer;
    D3D12_VERTEX_BUFFER_VIEW vbView = {};
    D3D12_INDEX_BUFFER_VIEW  ibView = {};

    // --- DXR BLAS リソース ---
    std::vector<ComPtr<ID3D12Resource>> blasBuffers;
};

struct alignas(4) MaterialConstants {
    uint32_t textureIndex;
    float    transmissionFactor;
    float    ior;
    float    baseColorFactor[4];
    uint32_t _pad;  // 32 bytes にアライメント
};