#include "pch.h"
#include "GraphicsContext.h"

#define CHECK(hr) if(FAILED(hr)) throw std::runtime_error("DX12 HRESULT failed")

bool GraphicsContext::Init(IUnknown* panelUnknown, int width, int height) {
    m_width = width; m_height = height;
    try {
        CreateDeviceAndQueue();
        CreateSwapChain(panelUnknown, width, height);
        CreateRTV();
        CreateDSV();
        for (UINT i = 0; i < FRAME_COUNT; i++)
            CHECK(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_cmdAllocators[i])));
        CHECK(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_cmdAllocators[0].Get(), nullptr, IID_PPV_ARGS(&m_cmdList)));
        CHECK(m_cmdList->Close());
        CHECK(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
        m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        m_currentFenceValue = 0;
        m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
        return true;
    }
    catch (...) { return false; }
}

void GraphicsContext::Shutdown() {
    WaitForGpu();
    if (m_fenceEvent) { CloseHandle(m_fenceEvent); m_fenceEvent = nullptr; }
}

void GraphicsContext::WaitForGpu() {
    m_currentFenceValue++;
    m_cmdQueue->Signal(m_fence.Get(), m_currentFenceValue);
    if (m_fence->GetCompletedValue() < m_currentFenceValue) {
        m_fence->SetEventOnCompletion(m_currentFenceValue, m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
}

void GraphicsContext::Resize(int width, int height, float scale) {
    if (width <= 0 || height <= 0) return;
    DXGI_MATRIX_3X2_F inverseScale = { 1.0f / scale, 0.0f, 0.0f, 1.0f / scale, 0.0f, 0.0f };
    m_swapChain->SetMatrixTransform(&inverseScale);
    if (width == m_width && height == m_height) return;
    WaitForGpu();
    for (auto& rt : m_renderTargets) rt.Reset();
    m_depthStencil.Reset();
    m_swapChain->ResizeBuffers(FRAME_COUNT, (UINT)width, (UINT)height, DXGI_FORMAT_R8G8B8A8_UNORM, 0);
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    m_width = width; m_height = height;

    // ウィンドウサイズ変更後にバッファを再作成した後、すべての状態追跡を PRESENT にリセット
    for (UINT i = 0; i < FRAME_COUNT; i++) {
        m_backBufferStates[i] = D3D12_RESOURCE_STATE_PRESENT;
    }

    CreateRTV();
    CreateDSV();
}

void GraphicsContext::ResetCommandList() {
    auto& alloc = m_cmdAllocators[m_frameIndex];
    alloc->Reset();
    m_cmdList->Reset(alloc.Get(), nullptr);

    // 状態が PRESENT の場合のみ遷移を実行
    if (m_backBufferStates[m_frameIndex] == D3D12_RESOURCE_STATE_PRESENT) {
        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
            m_renderTargets[m_frameIndex].Get(),
            D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        m_cmdList->ResourceBarrier(1, &barrier);
        m_backBufferStates[m_frameIndex] = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
}

void GraphicsContext::SetRenderTargetsAndClear(const float clearColor[4]) {
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtv(m_rtvHeap->GetCPUDescriptorHandleForHeapStart(), (INT)m_frameIndex, m_rtvDescSize);
    CD3DX12_CPU_DESCRIPTOR_HANDLE dsv(m_dsvHeap->GetCPUDescriptorHandleForHeapStart());

    m_cmdList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
    m_cmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    m_cmdList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
}

void GraphicsContext::ExecuteCommandListAndPresent() {
    // 状態が RENDER_TARGET の場合のみ PRESENT に戻す
    if (m_backBufferStates[m_frameIndex] == D3D12_RESOURCE_STATE_RENDER_TARGET) {
        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
            m_renderTargets[m_frameIndex].Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        m_cmdList->ResourceBarrier(1, &barrier);
        m_backBufferStates[m_frameIndex] = D3D12_RESOURCE_STATE_PRESENT;
    }
    m_cmdList->Close();

    ID3D12CommandList* lists[] = { m_cmdList.Get() };
    m_cmdQueue->ExecuteCommandLists(1, lists);
    m_swapChain->Present(1, 0);

    m_currentFenceValue++;
    m_cmdQueue->Signal(m_fence.Get(), m_currentFenceValue);
    m_fenceValues[m_frameIndex] = m_currentFenceValue;
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    if (m_fence->GetCompletedValue() < m_fenceValues[m_frameIndex]) {
        m_fence->SetEventOnCompletion(m_fenceValues[m_frameIndex], m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
}

// === 以下は元々 Renderer のプライベートハードウェア作成メソッドであり、そのまま移植 ===
void GraphicsContext::CreateDeviceAndQueue() {
#ifdef _DEBUG
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
#endif
    ComPtr<IDXGIFactory7> factory;
    CHECK(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
    CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device)));

    // --- ID3D12Device5 と DXR サポートを照会 ---
    if (SUCCEEDED(m_device.As(&m_device5))) {
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5 = {};
        if (SUCCEEDED(m_device5->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5)))) {
            m_isDxrSupported = (options5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_0);
        }
    }

    D3D12_COMMAND_QUEUE_DESC qDesc = {};
    qDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    CHECK(m_device->CreateCommandQueue(&qDesc, IID_PPV_ARGS(&m_cmdQueue)));
}

void GraphicsContext::CreateSwapChain(IUnknown* panelUnknown, int width, int height) {
    ComPtr<IDXGIFactory7> factory;
    CHECK(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = (UINT)width; desc.Height = (UINT)height;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferCount = FRAME_COUNT;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.SampleDesc = { 1, 0 };
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    ComPtr<IDXGISwapChain1> sc1;
    CHECK(factory->CreateSwapChainForComposition(m_cmdQueue.Get(), &desc, nullptr, &sc1));
    CHECK(sc1.As(&m_swapChain));
    ComPtr<ISwapChainPanelNative> panelNative;
    CHECK(panelUnknown->QueryInterface(IID_PPV_ARGS(&panelNative)));
    CHECK(panelNative->SetSwapChain(m_swapChain.Get()));
}

void GraphicsContext::CreateRTV() {
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.NumDescriptors = FRAME_COUNT;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    CHECK(m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_rtvHeap)));
    m_rtvDescSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_rtvHeap->GetCPUDescriptorHandleForHeapStart());
    for (UINT i = 0; i < FRAME_COUNT; i++) {
        CHECK(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_renderTargets[i])));
        m_device->CreateRenderTargetView(m_renderTargets[i].Get(), nullptr, rtvHandle);
        rtvHandle.Offset(1, m_rtvDescSize);
    }
}

void GraphicsContext::CreateDSV() {
    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
    dsvHeapDesc.NumDescriptors = 1;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    CHECK(m_device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&m_dsvHeap)));
    D3D12_CLEAR_VALUE optClear = {};
    optClear.Format = DXGI_FORMAT_D32_FLOAT;
    optClear.DepthStencil.Depth = 1.0f;
    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    auto texDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_D32_FLOAT, m_width, m_height, 1, 0, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    CHECK(m_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &texDesc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &optClear, IID_PPV_ARGS(&m_depthStencil)));
    m_device->CreateDepthStencilView(m_depthStencil.Get(), nullptr, m_dsvHeap->GetCPUDescriptorHandleForHeapStart());
}