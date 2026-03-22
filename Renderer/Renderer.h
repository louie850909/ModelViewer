#pragma once
#include "pch.h"
#include "Mesh.h"
#include <memory>
#include <mutex>

// 對應 shader 的 cbuffer 結構
struct SceneConstants {
    DirectX::XMFLOAT4X4 mvp;
    DirectX::XMFLOAT4X4 normalMatrix;
    DirectX::XMFLOAT3   cameraPos;
	float			    _pad1; // 填充對齊 (cbuffer 中的 float3 會自動對齊到 16 bytes，所以這裡補一個 float 當作填充)
    DirectX::XMFLOAT3   lightDir;
    float               _pad2;
    DirectX::XMFLOAT4   baseColor;
};

class Renderer {
public:
    std::mutex m_renderMutex;
    bool Init(IUnknown* panelUnknown, int width, int height);
    void Resize(int width, int height);
    void RenderFrame();
    void Shutdown();
    void UploadMeshToGpu(std::shared_ptr<Mesh> mesh); // ← 供 exports.cpp 呼叫
    void SetCameraTransform(float px, float py, float pz, float pitch, float yaw);

private:
    // --- 初始化相關 ---
    void CreateDeviceAndQueue();
    void CreateSwapChain(IUnknown* panelUnknown, int width, int height);
    void CreateRTV();
    void CreateDSV();
    void CreateRootSignatureAndPSO();  // ← 補上

    // --- 工具 ---
    void WaitForGpu();

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
    UINT   m_frameIndex = 0;
    HANDLE m_fenceEvent = nullptr;
    UINT64 m_fenceValues[FRAME_COUNT] = {};
    UINT64 m_currentFenceValue = 0;
    int    m_width = 0;
    int    m_height = 0;

    // --- 渲染管線 ---
    ComPtr<ID3D12RootSignature>       m_rootSig;
    ComPtr<ID3D12PipelineState>       m_psoOpaque;      // 不透明專用
    ComPtr<ID3D12PipelineState>       m_psoTransparent; // 半透明專用
    ComPtr<ID3D12Resource>            m_cbuffer;
    // Depth Buffer 相關資源
    ComPtr<ID3D12Resource>            m_depthStencil;
    ComPtr<ID3D12DescriptorHeap>      m_dsvHeap;
    UINT8* m_cbufferData = nullptr;

    // SRV (Shader Resource View) 相關資源 ...
    ComPtr<ID3D12DescriptorHeap> m_srvHeap;
    UINT m_srvDescriptorSize = 0;                   // 記錄 SRV 在 Heap 中的步長
    std::vector<ComPtr<ID3D12Resource>> m_textures; // 存放所有載入的貼圖
    ComPtr<ID3D12Resource>       m_textureUpload; // 負責將圖片資料從 CPU 搬運到 GPU 的中繼站

    // --- 網格 ---
    std::shared_ptr<Mesh>             m_mesh;
    ComPtr<ID3D12Resource>            m_vbUpload;  // 上傳完畢後保留避免 GPU 還在使用
    ComPtr<ID3D12Resource>            m_ibUpload;

    // 攝影機控制 (球座標參數)
    DirectX::XMFLOAT3 m_cameraPos = { 0.0f, 0.0f, -3.0f };
    float m_pitch = 0.0f;
    float m_yaw = 0.0f;
};