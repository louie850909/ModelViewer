#include "pch.h"
#include "TemporalDenoiserPass.h"

void TemporalDenoiserPass::Init(ID3D12Device* device) {
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.NumDescriptors = 4; // 1 UAV, 3 SRV
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_descriptorHeap));

    CD3DX12_DESCRIPTOR_RANGE1 uavRange;
    uavRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE);
    CD3DX12_DESCRIPTOR_RANGE1 srvRange;
    srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE);

    CD3DX12_ROOT_PARAMETER1 rootParams[3];
    rootParams[0].InitAsConstants(2, 0); // b0: width, height
    rootParams[1].InitAsDescriptorTable(1, &uavRange);
    rootParams[2].InitAsDescriptorTable(1, &srvRange);

    CD3DX12_STATIC_SAMPLER_DESC sampler(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR);
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rsDesc;
    rsDesc.Init_1_1(3, rootParams, 1, &sampler);

    ComPtr<ID3DBlob> sigBlob, errBlob;
    D3DX12SerializeVersionedRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1_1, &sigBlob, &errBlob);
    device->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&m_rootSig));

    ComPtr<ID3DBlob> csBlob;
    D3DReadFileToBlob(GetShaderPath(L"TemporalAccumulation.cso").c_str(), &csBlob);
    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = m_rootSig.Get();
    psoDesc.CS = CD3DX12_SHADER_BYTECODE(csBlob.Get());
    device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&m_pso));
}

void TemporalDenoiserPass::EnsureResources(ID3D12Device* device, int width, int height) {
    if (m_width == width && m_height == height && m_history[0] != nullptr) return;

    m_width = width;
    m_height = height;

    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    auto desc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, width, height, 1, 1);
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    // 建立一個暫用的 non-shader-visible heap 來做 Clear
    D3D12_DESCRIPTOR_HEAP_DESC clearHeapDesc = {};
    clearHeapDesc.NumDescriptors = 2;
    clearHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    clearHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE; // CPU-side only
    ComPtr<ID3D12DescriptorHeap> clearHeap;
    device->CreateDescriptorHeap(&clearHeapDesc, IID_PPV_ARGS(&clearHeap));
    UINT descSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    for (int i = 0; i < 2; ++i) {
        m_history[i].Reset();
        device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_history[i]));

        // 建立 CPU-side UAV descriptor，用於 ClearUnorderedAccessViewFloat
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uavDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;

        auto cpuHandle = CD3DX12_CPU_DESCRIPTOR_HANDLE(
            clearHeap->GetCPUDescriptorHandleForHeapStart(), i, descSize);
        device->CreateUnorderedAccessView(m_history[i].Get(), nullptr, &uavDesc, cpuHandle);
    }

    m_clearHeapForInit = clearHeap; // 暫存，供 Execute 第一幀使用
    m_needsClear = true;
}

void TemporalDenoiserPass::Execute(ID3D12GraphicsCommandList* cmdList, RenderPassContext& ctx) {
    auto device = ctx.gfx->GetDevice();
    EnsureResources(device, ctx.gfx->GetWidth(), ctx.gfx->GetHeight());

    if (!ctx.rawRaytracingOutput) return;

    if (m_needsClear) {
        m_needsClear = false;

        // 需要 shader-visible heap 的 GPU handle + non-visible heap 的 CPU handle 同時傳入
        // 用 m_descriptorHeap (shader-visible) 來建立 GPU handle
        UINT srvUavSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        ID3D12DescriptorHeap* heaps[] = { m_descriptorHeap.Get() };
        cmdList->SetDescriptorHeaps(1, heaps);

        const float clearColor[4] = { 0.f, 0.f, 0.f, 0.f };

        for (int i = 0; i < 2; ++i) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
            uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            uavDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;

            // 1. 取得 CPU-only heap 的 handle (用於 Clear 參數)
            auto clearCpuHandle = CD3DX12_CPU_DESCRIPTOR_HANDLE(
                m_clearHeapForInit->GetCPUDescriptorHandleForHeapStart(), i, srvUavSize);

            // 2. 取得 Shader-visible heap 的 handle (用於綁定與建立)
            auto cpuHandleInGpuHeap = CD3DX12_CPU_DESCRIPTOR_HANDLE(
                m_descriptorHeap->GetCPUDescriptorHandleForHeapStart(), i, srvUavSize);
            auto gpuHandle = CD3DX12_GPU_DESCRIPTOR_HANDLE(
                m_descriptorHeap->GetGPUDescriptorHandleForHeapStart(), i, srvUavSize);

            // 在 Shader-visible heap 中建立 UAV
            device->CreateUnorderedAccessView(m_history[i].Get(), nullptr, &uavDesc, cpuHandleInGpuHeap);

            // 執行 Clear (傳入對應的 gpuHandle 以及 CPU-only 的 clearCpuHandle)
            cmdList->ClearUnorderedAccessViewFloat(gpuHandle, clearCpuHandle,
                m_history[i].Get(), clearColor, 0, nullptr);
        }
    }

    // Ping-Pong 邏輯
    int readIdx = (ctx.frameCount) % 2;
    m_writeIdx = (ctx.frameCount + 1) % 2;

    auto rawGI = ctx.rawRaytracingOutput;
    auto velocity = ctx.gbuffer->GetVelocity();
    auto historyRead = m_history[readIdx].Get();
    auto historyWrite = m_history[m_writeIdx].Get();

    // 準備狀態 (轉為 SRV 供讀取)
    D3D12_RESOURCE_BARRIER preCompute[2] = {
        CD3DX12_RESOURCE_BARRIER::Transition(rawGI, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        CD3DX12_RESOURCE_BARRIER::Transition(historyRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
    };
    cmdList->ResourceBarrier(2, preCompute);

    UINT srvUavSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    CD3DX12_CPU_DESCRIPTOR_HANDLE cpuHandle(m_descriptorHeap->GetCPUDescriptorHandleForHeapStart());

    // 綁定 UAV (Output)
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uavDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    device->CreateUnorderedAccessView(historyWrite, nullptr, &uavDesc, cpuHandle);
    cpuHandle.Offset(1, srvUavSize);

    // 綁定 SRV (Raw GI, Velocity, History Read)
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(rawGI, &srvDesc, cpuHandle);
    cpuHandle.Offset(1, srvUavSize);

    srvDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
    device->CreateShaderResourceView(velocity, &srvDesc, cpuHandle);
    cpuHandle.Offset(1, srvUavSize);

    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    device->CreateShaderResourceView(historyRead, &srvDesc, cpuHandle);

    cmdList->SetPipelineState(m_pso.Get());
    cmdList->SetComputeRootSignature(m_rootSig.Get());

    ID3D12DescriptorHeap* heaps[] = { m_descriptorHeap.Get() };
    cmdList->SetDescriptorHeaps(1, heaps);

    uint32_t constants[2] = { (uint32_t)m_width, (uint32_t)m_height };
    cmdList->SetComputeRoot32BitConstants(0, 2, constants, 0);

    CD3DX12_GPU_DESCRIPTOR_HANDLE gpuHandle(m_descriptorHeap->GetGPUDescriptorHandleForHeapStart());
    cmdList->SetComputeRootDescriptorTable(1, gpuHandle); // UAV
    gpuHandle.Offset(1, srvUavSize);
    cmdList->SetComputeRootDescriptorTable(2, gpuHandle); // SRV 陣列

    cmdList->Dispatch((m_width + 7) / 8, (m_height + 7) / 8, 1);

    // 恢復狀態
    D3D12_RESOURCE_BARRIER postCompute[2] = {
        CD3DX12_RESOURCE_BARRIER::Transition(rawGI, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        CD3DX12_RESOURCE_BARRIER::Transition(historyRead, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    };
    cmdList->ResourceBarrier(2, postCompute);
}