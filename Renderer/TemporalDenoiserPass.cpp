#include "pch.h"
#include "TemporalDenoiserPass.h"

void TemporalDenoiserPass::Init(ID3D12Device* device) {
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    // 3 UAVs (GI, Normal, Pos) + 7 SRVs (RawGI, Vel, HistGI, CurNorm, CurPos, HistNorm, HistPos)
    heapDesc.NumDescriptors = 10;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_descriptorHeap));

    CD3DX12_DESCRIPTOR_RANGE1 uavRange;
    uavRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 3, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE);
    CD3DX12_DESCRIPTOR_RANGE1 srvRange;
    srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 7, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE);

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
    if (m_width == width && m_height == height && m_historyGI[0] != nullptr) return;

    m_width = width;
    m_height = height;

    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

    // 定義三種不同格式的 Buffer 資源
    auto descGI = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, width, height, 1, 1);
    descGI.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    auto descNormal = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT, width, height, 1, 1);
    descNormal.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    auto descPos = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32G32B32A32_FLOAT, width, height, 1, 1);
    descPos.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    for (int i = 0; i < 2; ++i) {
        m_historyGI[i].Reset();
        m_historyNormal[i].Reset();
        m_historyPos[i].Reset();
        device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &descGI, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_historyGI[i]));
        device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &descNormal, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_historyNormal[i]));
        device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &descPos, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_historyPos[i]));
    }
}

void TemporalDenoiserPass::Execute(ID3D12GraphicsCommandList* cmdList, RenderPassContext& ctx) {
    auto device = ctx.gfx->GetDevice();
    EnsureResources(device, ctx.gfx->GetWidth(), ctx.gfx->GetHeight());

    if (!ctx.rawRaytracingOutput) return;

    int readIdx = (ctx.frameCount) % 2;
    m_writeIdx = (ctx.frameCount + 1) % 2;

    auto rawGI = ctx.rawRaytracingOutput;

    // 狀態轉為 SRV (供讀取)
    D3D12_RESOURCE_BARRIER preCompute[4] = {
        CD3DX12_RESOURCE_BARRIER::Transition(rawGI, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        CD3DX12_RESOURCE_BARRIER::Transition(m_historyGI[readIdx].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        CD3DX12_RESOURCE_BARRIER::Transition(m_historyNormal[readIdx].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        CD3DX12_RESOURCE_BARRIER::Transition(m_historyPos[readIdx].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
    };
    cmdList->ResourceBarrier(4, preCompute);

    UINT srvUavSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    CD3DX12_CPU_DESCRIPTOR_HANDLE cpuHandle(m_descriptorHeap->GetCPUDescriptorHandleForHeapStart());

    // 綁定 3 個 UAV (Outputs)
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

    uavDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    device->CreateUnorderedAccessView(m_historyGI[m_writeIdx].Get(), nullptr, &uavDesc, cpuHandle); cpuHandle.Offset(1, srvUavSize);
    uavDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    device->CreateUnorderedAccessView(m_historyNormal[m_writeIdx].Get(), nullptr, &uavDesc, cpuHandle); cpuHandle.Offset(1, srvUavSize);
    uavDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    device->CreateUnorderedAccessView(m_historyPos[m_writeIdx].Get(), nullptr, &uavDesc, cpuHandle); cpuHandle.Offset(1, srvUavSize);

    // 綁定 7 個 SRV (Inputs)
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;

    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    device->CreateShaderResourceView(rawGI, &srvDesc, cpuHandle); cpuHandle.Offset(1, srvUavSize);
    srvDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
    device->CreateShaderResourceView(ctx.gbuffer->GetVelocity(), &srvDesc, cpuHandle); cpuHandle.Offset(1, srvUavSize);
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    device->CreateShaderResourceView(m_historyGI[readIdx].Get(), &srvDesc, cpuHandle); cpuHandle.Offset(1, srvUavSize);
    srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    device->CreateShaderResourceView(ctx.gbuffer->GetNormal(), &srvDesc, cpuHandle); cpuHandle.Offset(1, srvUavSize);
    srvDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    device->CreateShaderResourceView(ctx.gbuffer->GetWorldPos(), &srvDesc, cpuHandle); cpuHandle.Offset(1, srvUavSize);
    srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    device->CreateShaderResourceView(m_historyNormal[readIdx].Get(), &srvDesc, cpuHandle); cpuHandle.Offset(1, srvUavSize);
    srvDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    device->CreateShaderResourceView(m_historyPos[readIdx].Get(), &srvDesc, cpuHandle);

    cmdList->SetPipelineState(m_pso.Get());
    cmdList->SetComputeRootSignature(m_rootSig.Get());

    ID3D12DescriptorHeap* heaps[] = { m_descriptorHeap.Get() };
    cmdList->SetDescriptorHeaps(1, heaps);

    uint32_t constants[2] = { (uint32_t)m_width, (uint32_t)m_height };
    cmdList->SetComputeRoot32BitConstants(0, 2, constants, 0);

    CD3DX12_GPU_DESCRIPTOR_HANDLE gpuHandle(m_descriptorHeap->GetGPUDescriptorHandleForHeapStart());
    cmdList->SetComputeRootDescriptorTable(1, gpuHandle); // UAV table
    gpuHandle.Offset(3, srvUavSize);
    cmdList->SetComputeRootDescriptorTable(2, gpuHandle); // SRV table starts at offset 3

    cmdList->Dispatch((m_width + 7) / 8, (m_height + 7) / 8, 1);

    // 恢復狀態
    D3D12_RESOURCE_BARRIER postCompute[4] = {
        CD3DX12_RESOURCE_BARRIER::Transition(rawGI, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        CD3DX12_RESOURCE_BARRIER::Transition(m_historyGI[readIdx].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        CD3DX12_RESOURCE_BARRIER::Transition(m_historyNormal[readIdx].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        CD3DX12_RESOURCE_BARRIER::Transition(m_historyPos[readIdx].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    };
    cmdList->ResourceBarrier(4, postCompute);
}