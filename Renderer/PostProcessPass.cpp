#include "pch.h"
#include "PostProcessPass.h"

void PostProcessPass::Init(ID3D12Device* device) {
    // Descriptor Heap を作成 (1 SRV, 1 UAV)
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.NumDescriptors = 2;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_srvUavHeap));

    // Root Signature: b0 (Constants), t0 (SRV), u0 (UAV)
    CD3DX12_DESCRIPTOR_RANGE1 ranges[2];
    ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0); // t0
    ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0); // u0

    CD3DX12_ROOT_PARAMETER1 rootParams[3];
    rootParams[0].InitAsConstants(4, 0); // b0
    rootParams[1].InitAsDescriptorTable(1, &ranges[0]);
    rootParams[2].InitAsDescriptorTable(1, &ranges[1]);

    CD3DX12_STATIC_SAMPLER_DESC sampler(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR);
    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rsDesc;
    rsDesc.Init_1_1(3, rootParams, 1, &sampler);

    ComPtr<ID3DBlob> sigBlob, errBlob;
    D3DX12SerializeVersionedRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1_1, &sigBlob, &errBlob);
    device->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&m_rootSig));

    // 既存の Shader 読み取り機構を使用
    ComPtr<ID3DBlob> csBlob;
    D3DReadFileToBlob(GetShaderPath(L"PostProcess_CAS.cso").c_str(), &csBlob);

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = m_rootSig.Get();
    psoDesc.CS = CD3DX12_SHADER_BYTECODE(csBlob.Get());
    device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&m_pso));
}

void PostProcessPass::EnsureResources(ID3D12Device* device, int width, int height) {
    if (m_width == width && m_height == height && m_uavOutput) return;
    m_width = width; m_height = height;

    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    auto desc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, width, height, 1, 1);
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr, IID_PPV_ARGS(&m_uavOutput));
}

void PostProcessPass::Execute(ID3D12GraphicsCommandList* cmdList, RenderPassContext& ctx) {
    ID3D12Resource* input = ctx.rawDiffuseGI; // ここは SpatialPass の出力リソースであるべき
    if (!input) return;

    EnsureResources(ctx.gfx->GetDevice(), ctx.gfx->GetWidth(), ctx.gfx->GetHeight());

    // 状態遷移
    D3D12_RESOURCE_BARRIER barriers[2] = {
        CD3DX12_RESOURCE_BARRIER::Transition(input, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        CD3DX12_RESOURCE_BARRIER::Transition(m_uavOutput.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    };
    cmdList->ResourceBarrier(2, barriers);

    // リソースをバインド
    UINT handleSize = ctx.gfx->GetDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    CD3DX12_CPU_DESCRIPTOR_HANDLE cpuHandle(m_srvUavHeap->GetCPUDescriptorHandleForHeapStart());

    // Create SRV & UAV
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = input->GetDesc().Format;
    srvDesc.Texture2D.MipLevels = 1;
    ctx.gfx->GetDevice()->CreateShaderResourceView(input, &srvDesc, cpuHandle);
    cpuHandle.Offset(1, handleSize);

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uavDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    ctx.gfx->GetDevice()->CreateUnorderedAccessView(m_uavOutput.Get(), nullptr, &uavDesc, cpuHandle);

    // Dispatch
    cmdList->SetPipelineState(m_pso.Get());
    cmdList->SetComputeRootSignature(m_rootSig.Get());
    ID3D12DescriptorHeap* heaps[] = { m_srvUavHeap.Get() };
    cmdList->SetDescriptorHeaps(1, heaps);

    uint32_t constants[4] = { *(uint32_t*)&m_sharpness, (uint32_t)m_width, (uint32_t)m_height, 0 };
    cmdList->SetComputeRoot32BitConstants(0, 4, constants, 0);

    CD3DX12_GPU_DESCRIPTOR_HANDLE gpuHandle(m_srvUavHeap->GetGPUDescriptorHandleForHeapStart());
    cmdList->SetComputeRootDescriptorTable(1, gpuHandle);
    gpuHandle.Offset(1, handleSize);
    cmdList->SetComputeRootDescriptorTable(2, gpuHandle);

    cmdList->Dispatch((m_width + 7) / 8, (m_height + 7) / 8, 1);

    // 最後：結果を BackBuffer にコピー
    auto backBuffer = ctx.gfx->GetCurrentBackBuffer();
    D3D12_RESOURCE_BARRIER copyBarriers[2] = {
        CD3DX12_RESOURCE_BARRIER::Transition(m_uavOutput.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
        CD3DX12_RESOURCE_BARRIER::Transition(backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST)
    };
    cmdList->ResourceBarrier(2, copyBarriers);
    cmdList->CopyResource(backBuffer, m_uavOutput.Get());

    // 後続の UI レンダリングのために RenderTarget に戻す
    auto finalBarrier = CD3DX12_RESOURCE_BARRIER::Transition(backBuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmdList->ResourceBarrier(1, &finalBarrier);
}