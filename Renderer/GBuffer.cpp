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
    // 古いリソースを解放
    m_albedo.Reset();
    m_normalRouness.Reset();
    m_worldPosMetallic.Reset();

    // 再作成
    CreateResources(device, width, height);
    CreateHeapsAndViews(device);
}

void GBuffer::Shutdown() {
    m_albedo.Reset();
    m_normalRouness.Reset();
    m_worldPosMetallic.Reset();
    m_rtvHeap.Reset();
    m_srvHeap.Reset();
}

void GBuffer::CreateResources(ID3D12Device* device, int width, int height) {
    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

    // SRV をデフォルト初期状態に設定
    D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    // 1. Albedo (R8G8B8A8)
    D3D12_CLEAR_VALUE clearColorZero = {};
    clearColorZero.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    clearColorZero.Color[0] = 0.0f; clearColorZero.Color[1] = 0.0f; clearColorZero.Color[2] = 0.0f; clearColorZero.Color[3] = 0.0f;
    auto descAlbedo = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, width, height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    CHECK(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &descAlbedo, initialState, &clearColorZero, IID_PPV_ARGS(&m_albedo)));

    // 2. Normal + Roughness (R16G16B16A16_FLOAT) - 法線は負の値が必要なため FLOAT を使用
    D3D12_CLEAR_VALUE clearNormal = {};
    clearNormal.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    clearNormal.Color[0] = 0.0f; clearNormal.Color[1] = 0.0f; clearNormal.Color[2] = 0.0f; clearNormal.Color[3] = 0.0f;
    auto descNormal = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT, width, height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    CHECK(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &descNormal, initialState, &clearNormal, IID_PPV_ARGS(&m_normalRouness)));

    // 3. World Position + Metallic (R32G32B32A32_FLOAT) - ワールド座標の範囲が大きいため 32-bit FLOAT が必要
    D3D12_CLEAR_VALUE clearPos = {};
    clearPos.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    clearPos.Color[0] = 0.0f; clearPos.Color[1] = 0.0f; clearPos.Color[2] = 0.0f; clearPos.Color[3] = 0.0f;
    auto descPos = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32G32B32A32_FLOAT, width, height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    CHECK(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &descPos, initialState, &clearPos, IID_PPV_ARGS(&m_worldPosMetallic)));

    // 4. Velocity (R16G16_FLOAT) - スクリーン UV 空間の移動ベクトルを格納するのに十分
    D3D12_CLEAR_VALUE clearVel = {};
    clearVel.Format = DXGI_FORMAT_R16G16_FLOAT;
    clearVel.Color[0] = 0.0f; clearVel.Color[1] = 0.0f; clearVel.Color[2] = 0.0f; clearVel.Color[3] = 0.0f;
    auto descVel = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16G16_FLOAT, width, height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    CHECK(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &descVel, initialState, &clearVel, IID_PPV_ARGS(&m_velocity)));
}

void GBuffer::CreateHeapsAndViews(ID3D12Device* device) {
    // RTV Heap を作成 (Geometry Pass の書き込み用)
    if (!m_rtvHeap) {
        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
        rtvHeapDesc.NumDescriptors = TargetCount;
        rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        CHECK(device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap)));
    }

    // SRV Heap を作成 (Lighting Pass の読み取り用、SHADER_VISIBLE 必須)
    if (!m_srvHeap) {
        D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
        srvHeapDesc.NumDescriptors = TargetCount;
        srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        CHECK(device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&m_srvHeap)));
    }

    // RTV をバインド
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_rtvHeap->GetCPUDescriptorHandleForHeapStart());
    device->CreateRenderTargetView(m_albedo.Get(), nullptr, rtvHandle);
    rtvHandle.Offset(1, m_rtvDescSize);
    device->CreateRenderTargetView(m_normalRouness.Get(), nullptr, rtvHandle);
    rtvHandle.Offset(1, m_rtvDescSize);
    device->CreateRenderTargetView(m_worldPosMetallic.Get(), nullptr, rtvHandle);
    rtvHandle.Offset(1, m_rtvDescSize);
    device->CreateRenderTargetView(m_velocity.Get(), nullptr, rtvHandle);

    // SRV をバインド
    CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle(m_srvHeap->GetCPUDescriptorHandleForHeapStart());
    device->CreateShaderResourceView(m_albedo.Get(), nullptr, srvHandle);
    srvHandle.Offset(1, m_srvDescSize);
    device->CreateShaderResourceView(m_normalRouness.Get(), nullptr, srvHandle);
    srvHandle.Offset(1, m_srvDescSize);
    device->CreateShaderResourceView(m_worldPosMetallic.Get(), nullptr, srvHandle);
    srvHandle.Offset(1, m_srvDescSize);
    device->CreateShaderResourceView(m_velocity.Get(), nullptr, srvHandle);
}