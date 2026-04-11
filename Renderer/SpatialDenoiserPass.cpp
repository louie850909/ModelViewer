#include "pch.h"
#include "SpatialDenoiserPass.h"

void SpatialDenoiserPass::Init(ID3D12Device* device) {
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    // 4 個 Pass，每個 Pass 需要 1 UAV + 3 SRV = 16 個描述子。開 32 個確保充足。
    heapDesc.NumDescriptors = 32;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_descriptorHeap));

    CD3DX12_DESCRIPTOR_RANGE1 uavRange;
    uavRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE);
    CD3DX12_DESCRIPTOR_RANGE1 srvRange;
    srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE);

    CD3DX12_ROOT_PARAMETER1 rootParams[3];
    // 增加一個 Constant 參數供 stepSize 使用
    rootParams[0].InitAsConstants(3, 0);
    rootParams[1].InitAsDescriptorTable(1, &uavRange);
    rootParams[2].InitAsDescriptorTable(1, &srvRange);

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rsDesc;
    rsDesc.Init_1_1(3, rootParams, 0, nullptr);

    ComPtr<ID3DBlob> sigBlob, errBlob;
    D3DX12SerializeVersionedRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1_1, &sigBlob, &errBlob);
    device->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&m_rootSig));

    ComPtr<ID3DBlob> csBlob;
    D3DReadFileToBlob(GetShaderPath(L"SpatialFilter.cso").c_str(), &csBlob);
    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = m_rootSig.Get();
    psoDesc.CS = CD3DX12_SHADER_BYTECODE(csBlob.Get());
    device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&m_pso));
}

void SpatialDenoiserPass::EnsureResources(ID3D12Device* device, int width, int height) {
    if (m_width == width && m_height == height && m_pingPongBuffers[0] != nullptr) return;
    m_width = width; m_height = height;

    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    auto desc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, width, height, 1, 1);
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    for (int i = 0; i < 2; ++i) {
        m_pingPongBuffers[i].Reset();
        device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_pingPongBuffers[i]));
    }
}

void SpatialDenoiserPass::Execute(ID3D12GraphicsCommandList* cmdList, RenderPassContext& ctx) {
    if (!m_temporalPass) return;
    auto device = ctx.gfx->GetDevice();
    EnsureResources(device, ctx.gfx->GetWidth(), ctx.gfx->GetHeight());

    auto inputGI = m_temporalPass->GetDenoisedOutput();
    auto normalMap = ctx.gbuffer->GetNormal();
    auto worldPosMap = ctx.gbuffer->GetWorldPos();

    // 初始狀態轉換
    D3D12_RESOURCE_BARRIER preCompute[1] = {
        CD3DX12_RESOURCE_BARRIER::Transition(inputGI, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
    };
    cmdList->ResourceBarrier(1, preCompute);

    ID3D12Resource* srvRes = inputGI;
    ID3D12Resource* uavRes = m_pingPongBuffers[0].Get();

    UINT srvUavSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    CD3DX12_CPU_DESCRIPTOR_HANDLE cpuHandle(m_descriptorHeap->GetCPUDescriptorHandleForHeapStart());
    CD3DX12_GPU_DESCRIPTOR_HANDLE gpuHandle(m_descriptorHeap->GetGPUDescriptorHandleForHeapStart());

    cmdList->SetPipelineState(m_pso.Get());
    cmdList->SetComputeRootSignature(m_rootSig.Get());
    ID3D12DescriptorHeap* heaps[] = { m_descriptorHeap.Get() };
    cmdList->SetDescriptorHeaps(1, heaps);

    // 執行 4 個 Pass 的 À-Trous 小波降噪
    int numPasses = 4;
    for (int i = 0; i < numPasses; ++i) {
        int stepSize = 1 << i; // 1, 2, 4, 8

        // 綁定 UAV
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uavDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        device->CreateUnorderedAccessView(uavRes, nullptr, &uavDesc, cpuHandle);
        auto uavGpuHandle = gpuHandle;
        cpuHandle.Offset(1, srvUavSize); gpuHandle.Offset(1, srvUavSize);

        // 綁定 3 個 SRV
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;
        auto srvGpuHandle = gpuHandle;

        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        device->CreateShaderResourceView(srvRes, &srvDesc, cpuHandle);
        cpuHandle.Offset(1, srvUavSize); gpuHandle.Offset(1, srvUavSize);

        srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        device->CreateShaderResourceView(normalMap, &srvDesc, cpuHandle);
        cpuHandle.Offset(1, srvUavSize); gpuHandle.Offset(1, srvUavSize);

        srvDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        device->CreateShaderResourceView(worldPosMap, &srvDesc, cpuHandle);
        cpuHandle.Offset(1, srvUavSize); gpuHandle.Offset(1, srvUavSize);

        uint32_t constants[3] = { (uint32_t)m_width, (uint32_t)m_height, (uint32_t)stepSize };
        cmdList->SetComputeRoot32BitConstants(0, 3, constants, 0);
        cmdList->SetComputeRootDescriptorTable(1, uavGpuHandle);
        cmdList->SetComputeRootDescriptorTable(2, srvGpuHandle);

        cmdList->Dispatch((m_width + 7) / 8, (m_height + 7) / 8, 1);

        // 管理 Ping-Pong 狀態轉換
        if (i < numPasses - 1) {
            int numBarriers = 0;
            D3D12_RESOURCE_BARRIER bars[2];
            // 剛寫完的 UAV 變成下一輪的 SRV
            bars[numBarriers++] = CD3DX12_RESOURCE_BARRIER::Transition(uavRes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            // 剛讀完的 SRV (若不是原始 input) 轉回 UAV 供下一輪寫入
            if (srvRes != inputGI) {
                bars[numBarriers++] = CD3DX12_RESOURCE_BARRIER::Transition(srvRes, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
            cmdList->ResourceBarrier(numBarriers, bars);

            // 互換讀寫指標
            auto temp = srvRes;
            srvRes = uavRes;
            uavRes = (temp == inputGI) ? m_pingPongBuffers[1].Get() : temp;
        }
    }

    // 恢復狀態
    D3D12_RESOURCE_BARRIER postCompute[2] = {
        CD3DX12_RESOURCE_BARRIER::Transition(inputGI, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        CD3DX12_RESOURCE_BARRIER::Transition(srvRes, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    };
    cmdList->ResourceBarrier(2, postCompute);

    // 最後一次寫入的 uavRes 即為完成品
    auto finalOutput = uavRes;
    auto backBuffer = ctx.gfx->GetCurrentBackBuffer();

    D3D12_RESOURCE_BARRIER copyBarriers[2] = {
        CD3DX12_RESOURCE_BARRIER::Transition(backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST),
        CD3DX12_RESOURCE_BARRIER::Transition(finalOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE)
    };
    cmdList->ResourceBarrier(2, copyBarriers);

    cmdList->CopyResource(backBuffer, finalOutput);

    D3D12_RESOURCE_BARRIER copyBarriersPost[2] = {
        CD3DX12_RESOURCE_BARRIER::Transition(backBuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET),
        CD3DX12_RESOURCE_BARRIER::Transition(finalOutput, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    };
    cmdList->ResourceBarrier(2, copyBarriersPost);
}