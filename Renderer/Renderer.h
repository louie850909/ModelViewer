#pragma once
#include "pch.h"
#include "Mesh.h"
#include <memory>
#include <mutex>
#include <atomic>
#include <chrono>
#include <vector>

// 對應 shader 的 cbuffer 結構
struct SceneConstants {
    DirectX::XMFLOAT4X4 mvp;
    DirectX::XMFLOAT4X4 modelMatrix;
    DirectX::XMFLOAT4X4 normalMatrix;
    DirectX::XMFLOAT3   cameraPos;
    float               _pad1;
    DirectX::XMFLOAT3   lightDir;
    float               _pad2;
    DirectX::XMFLOAT4   baseColor;
};
// sizeof(SceneConstants) = 240 bytes = 60 floats

// 場景中一個模型的封裝
struct MeshInstance {
    int                    meshId = -1;
    std::shared_ptr<Mesh>  mesh;

    // 這個模型專屬的 SRV Heap 與負責上傳尋路的尊上源
    ComPtr<ID3D12DescriptorHeap>         srvHeap;
    std::vector<ComPtr<ID3D12Resource>>  textures;
    ComPtr<ID3D12Resource>               vbUpload;
    ComPtr<ID3D12Resource>               ibUpload;
};

// Node 全域 index 編碼：
// globalIndex = meshId * MESH_NODE_STRIDE + localNodeIndex
// MESH_NODE_STRIDE 必須大於任何單一模型的節點數
constexpr int MESH_NODE_STRIDE = 10000;

class Renderer {
public:
    std::mutex m_renderMutex;

    bool Init(IUnknown* panelUnknown, int width, int height);
    void Resize(int width, int height, float scale);
    void RenderFrame();
    void Shutdown();

    // 新增模型到場景，回傳 meshId；失敗回傳 -1
    int  AddMesh(std::shared_ptr<Mesh> mesh);
    // 從場景移除指定模型
    void RemoveMeshById(int meshId);

    // 相機
    void SetCameraTransform(float px, float py, float pz, float pitch, float yaw);

    // 統計
    void GetStats(int& vertices, int& polygons, int& drawCalls, float& frameTimeMs) {
        vertices  = m_statVertices.load(std::memory_order_relaxed);
        polygons  = m_statPolygons.load(std::memory_order_relaxed);
        drawCalls = m_statDrawCalls.load(std::memory_order_relaxed);
        frameTimeMs = m_statFrameTime.load(std::memory_order_relaxed);
    }

    // Node 全域存取（globalIndex = meshId * MESH_NODE_STRIDE + localIdx）
    int  GetTotalNodeCount();
    bool GetNodeInfo(int globalIndex, std::string& outName, int& outParentGlobal);
    bool GetNodeTransform(int globalIndex, float* outT, float* outR, float* outS);
    bool SetNodeTransform(int globalIndex, const float* inT, const float* inR, const float* inS);

    // 相容舊 API：取得第一個 mesh（如果存在）
    std::shared_ptr<Mesh> GetMesh() const {
        return m_meshes.empty() ? nullptr : m_meshes[0].mesh;
    }

    // GPU 上傳（由 export.cpp 的背景 thread 呼叫）
    void UploadMeshToGpu(std::shared_ptr<Mesh> mesh, int meshId);

private:
    void CreateDeviceAndQueue();
    void CreateSwapChain(IUnknown* panelUnknown, int width, int height);
    void CreateRTV();
    void CreateDSV();
    void CreateRootSignatureAndPSO();
    void WaitForGpu();

    // 內部輿助：由 globalIndex 找到 MeshInstance + localIndex
    MeshInstance* FindInstance(int globalIndex, int& outLocalIndex);

    // 下一個可用 meshId
    int m_nextMeshId = 0;

    // 場景內所有模型
    std::vector<MeshInstance> m_meshes;

    // --- DX12 核心 ---
    static constexpr UINT FRAME_COUNT = 3;

    ComPtr<ID3D12Device>              m_device;
    ComPtr<ID3D12CommandQueue>        m_cmdQueue;
    ComPtr<IDXGISwapChain4>           m_swapChain;
    ComPtr<ID3D12DescriptorHeap>      m_rtvHeap;
    ComPtr<ID3D12Resource>            m_renderTargets[FRAME_COUNT];
    ComPtr<ID3D12CommandAllocator>    m_cmdAllocators[FRAME_COUNT];
    ComPtr<ID3D12GraphicsCommandList> m_cmdList;
    ComPtr<ID3D12Fence>               m_fence;

    UINT   m_rtvDescSize = 0;
    UINT   m_srvDescriptorSize = 0;
    UINT   m_frameIndex = 0;
    HANDLE m_fenceEvent = nullptr;
    UINT64 m_fenceValues[FRAME_COUNT] = {};
    UINT64 m_currentFenceValue = 0;
    int    m_width = 0;
    int    m_height = 0;

    ComPtr<ID3D12RootSignature>       m_rootSig;
    ComPtr<ID3D12PipelineState>       m_psoOpaque;
    ComPtr<ID3D12PipelineState>       m_psoTransparent;
    ComPtr<ID3D12Resource>            m_depthStencil;
    ComPtr<ID3D12DescriptorHeap>      m_dsvHeap;

    // 相機
    DirectX::XMFLOAT3 m_cameraPos = { 0.0f, 0.0f, -3.0f };
    float m_pitch = 0.0f;
    float m_yaw   = 0.0f;

    // 效能統計
    std::atomic<int>   m_statVertices{ 0 };
    std::atomic<int>   m_statPolygons{ 0 };
    std::atomic<int>   m_statDrawCalls{ 0 };
    std::atomic<float> m_statFrameTime{ 0.0f };
    std::chrono::high_resolution_clock::time_point m_lastFrameTime;
};
