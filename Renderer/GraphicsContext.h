#pragma once
#include "pch.h"
#include <stdexcept>

class GraphicsContext {
public:
    bool Init(IUnknown* panelUnknown, int width, int height);
    void Shutdown();
    void Resize(int width, int height, float scale);
    void WaitForGpu();

    ID3D12Device5* GetDevice5() const { return m_device5.Get(); }
    bool IsDxrSupported() const { return m_isDxrSupported; }

    // 取得底層資源
    ID3D12Device* GetDevice() const { return m_device.Get(); }
    ID3D12GraphicsCommandList* GetCommandList() const { return m_cmdList.Get(); }
    ID3D12CommandQueue* GetCommandQueue() const { return m_cmdQueue.Get(); }

    // 取得當前 BackBuffer 的 RTV
    D3D12_CPU_DESCRIPTOR_HANDLE GetCurrentBackBufferRTV() const {
        return CD3DX12_CPU_DESCRIPTOR_HANDLE(m_rtvHeap->GetCPUDescriptorHandleForHeapStart(), m_frameIndex, m_rtvDescSize);
    }

    // --- 取得目前的 Back Buffer 資源 ---
    ID3D12Resource* GetCurrentBackBuffer() const {
        // 註：請確認此處的變數名稱與您的 GraphicsContext 宣告一致
        // 若您的陣列命名為 m_backBuffers，請對應修改
        return m_renderTargets[m_frameIndex].Get();
    }
    // 取得 Depth Stencil View
    D3D12_CPU_DESCRIPTOR_HANDLE GetDSV() const {
        return m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    }

    // 渲染迴圈控制
    void ResetCommandList();
    void SetRenderTargetsAndClear(const float clearColor[4]);
    void ExecuteCommandListAndPresent();

    D3D12_VIEWPORT GetViewport() const { return { 0, 0, (float)m_width, (float)m_height, 0, 1 }; }
    D3D12_RECT GetScissorRect() const { return { 0, 0, m_width, m_height }; }

    int GetWidth() const { return m_width; }
    int GetHeight() const { return m_height; }

private:
    void CreateDeviceAndQueue();
    void CreateSwapChain(IUnknown* panelUnknown, int width, int height);
    void CreateRTV();
    void CreateDSV();

    static constexpr UINT FRAME_COUNT = 3;
    ComPtr<ID3D12Device>              m_device;
    ComPtr<ID3D12Device5>             m_device5;
    bool                              m_isDxrSupported = false;
    ComPtr<ID3D12CommandQueue>        m_cmdQueue;
    ComPtr<IDXGISwapChain4>           m_swapChain;
    ComPtr<ID3D12DescriptorHeap>      m_rtvHeap;
    ComPtr<ID3D12Resource>            m_renderTargets[FRAME_COUNT];

    // 追蹤每個 BackBuffer 的當前狀態
    D3D12_RESOURCE_STATES m_backBufferStates[FRAME_COUNT] = {
        D3D12_RESOURCE_STATE_PRESENT,
        D3D12_RESOURCE_STATE_PRESENT,
        D3D12_RESOURCE_STATE_PRESENT
    };

    ComPtr<ID3D12CommandAllocator>    m_cmdAllocators[FRAME_COUNT];
    ComPtr<ID3D12GraphicsCommandList> m_cmdList;
    ComPtr<ID3D12Fence>               m_fence;
    ComPtr<ID3D12Resource>            m_depthStencil;
    ComPtr<ID3D12DescriptorHeap>      m_dsvHeap;

    UINT m_rtvDescSize = 0;
    UINT m_frameIndex = 0;
    HANDLE m_fenceEvent = nullptr;
    UINT64 m_fenceValues[FRAME_COUNT] = {};
    UINT64 m_currentFenceValue = 0;
    int m_width = 0;
    int m_height = 0;
};
