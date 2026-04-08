#include "pch.h"
#include "GBuffer.h"

#define CHECK(hr) if(FAILED(hr)) throw std::runtime_error("DX12 GBuffer failed")

void GBuffer::Init(ID3D12Device* device, int width, int height) {
    m_rtvDescSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    m_srvDescSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    CreateResources(device, width, height);
    CreateHeapsAndViews(device);
}

void GBuffer::Resize(ID3D12Device* device, int width, int height) {
    // 釋放舊資源
    m_albedo.Reset();
    m_normal.Reset();
    m_worldPos.Reset();

    // 重新建立
    CreateResources(device, width, height);
    CreateHeapsAndViews(device);
}

void GBuffer::Shutdown() {
    m_albedo.Reset();
    m_normal.Reset();
    m_worldPos.Reset();
    m_rtvHeap.Reset();
    m_srvHeap.Reset();
}

void GBuffer::CreateResources(ID3D12Device* device, int width, int height) {
    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

    // 將 SRV 設為預設初始狀態
    D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    // 1. Albedo + Roughness (R8G8B8A8)
    D3D12_CLEAR_VALUE clearColorZero = {};
    clearColorZero.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    clearColorZero.Color[0] = 0.0f; clearColorZero.Color[1] = 0.0f; clearColorZero.Color[2] = 0.0f; clearColorZero.Color[3] = 0.0f;
    auto descAlbedo = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, width, height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    CHECK(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &descAlbedo, initialState, &clearColorZero, IID_PPV_ARGS(&m_albedo)));

    // 2. Normal + Metallic (R16G16B16A16_FLOAT) - 法線需要負值，所以用 FLOAT
    D3D12_CLEAR_VALUE clearNormal = {};
    clearNormal.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    clearNormal.Color[0] = 0.0f; clearNormal.Color[1] = 0.0f; clearNormal.Color[2] = 0.0f; clearNormal.Color[3] = 0.0f;
    auto descNormal = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT, width, height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    CHECK(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &descNormal, initialState, &clearNormal, IID_PPV_ARGS(&m_normal)));

    // 3. World Position (R32G32B32A32_FLOAT) - 世界座標範圍很大，需要 32-bit FLOAT
    D3D12_CLEAR_VALUE clearPos = {};
    clearPos.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    clearPos.Color[0] = 0.0f; clearPos.Color[1] = 0.0f; clearPos.Color[2] = 0.0f; clearPos.Color[3] = 0.0f;
    auto descPos = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32G32B32A32_FLOAT, width, height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    CHECK(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &descPos, initialState, &clearPos, IID_PPV_ARGS(&m_worldPos)));

    // 4. Velocity (R16G16_FLOAT) - 足以存放螢幕 UV 空間的移動向量
    D3D12_CLEAR_VALUE clearVel = {};
    clearVel.Format = DXGI_FORMAT_R16G16_FLOAT;
    clearVel.Color[0] = 0.0f; clearVel.Color[1] = 0.0f; clearVel.Color[2] = 0.0f; clearVel.Color[3] = 0.0f;
    auto descVel = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16G16_FLOAT, width, height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    CHECK(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &descVel, initialState, &clearVel, IID_PPV_ARGS(&m_velocity)));
}

void GBuffer::CreateHeapsAndViews(ID3D12Device* device) {
    // 建立 RTV Heap (供 Geometry Pass 寫入)
    if (!m_rtvHeap) {
        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
        rtvHeapDesc.NumDescriptors = TargetCount;
        rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        CHECK(device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap)));
    }

    // 建立 SRV Heap (供 Lighting Pass 讀取，必須為 SHADER_VISIBLE)
    if (!m_srvHeap) {
        D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
        srvHeapDesc.NumDescriptors = TargetCount;
        srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        CHECK(device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&m_srvHeap)));
    }

    // 綁定 RTV
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_rtvHeap->GetCPUDescriptorHandleForHeapStart());
    device->CreateRenderTargetView(m_albedo.Get(), nullptr, rtvHandle);
    rtvHandle.Offset(1, m_rtvDescSize);
    device->CreateRenderTargetView(m_normal.Get(), nullptr, rtvHandle);
    rtvHandle.Offset(1, m_rtvDescSize);
    device->CreateRenderTargetView(m_worldPos.Get(), nullptr, rtvHandle);
    rtvHandle.Offset(1, m_rtvDescSize);
    device->CreateRenderTargetView(m_velocity.Get(), nullptr, rtvHandle);

    // 綁定 SRV
    CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle(m_srvHeap->GetCPUDescriptorHandleForHeapStart());
    device->CreateShaderResourceView(m_albedo.Get(), nullptr, srvHandle);
    srvHandle.Offset(1, m_srvDescSize);
    device->CreateShaderResourceView(m_normal.Get(), nullptr, srvHandle);
    srvHandle.Offset(1, m_srvDescSize);
    device->CreateShaderResourceView(m_worldPos.Get(), nullptr, srvHandle);
    srvHandle.Offset(1, m_srvDescSize);
    device->CreateShaderResourceView(m_velocity.Get(), nullptr, srvHandle);
}