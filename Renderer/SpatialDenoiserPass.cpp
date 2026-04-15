#include "pch.h"
#include "SpatialDenoiserPass.h"

void SpatialDenoiserPass::Init(ID3D12Device* device) {
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.NumDescriptors = 40; // 安全のために拡大
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_descriptorHeap));

    CD3DX12_DESCRIPTOR_RANGE1 uavRange;
    uavRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE);
    CD3DX12_DESCRIPTOR_RANGE1 srvRange;
    srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 6, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE); // ★ 6 つの SRV

    CD3DX12_ROOT_PARAMETER1 rootParams[3];
    rootParams[0].InitAsConstants(5, 0);
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
    if (m_width == width && m_height == height && m_pingPongDiffuse[0] != nullptr) return;
    m_width = width; m_height = height;

    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

    // Ping-Pong テクスチャはデノイズ演算のため HDR を維持
    auto descHDR = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT, width, height, 1, 1);
    descHDR.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    // 最終出力テクスチャは UNORM で BackBuffer との互換性を担う
    auto descLDR = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, width, height, 1, 1);
    descLDR.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    for (int i = 0; i < 2; ++i) {
        m_pingPongDiffuse[i].Reset();
        m_pingPongSpecular[i].Reset();
        device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &descHDR, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_pingPongDiffuse[i]));
        device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &descHDR, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_pingPongSpecular[i]));
    }

    m_finalOutput.Reset();
    device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &descLDR, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_finalOutput));
}

void SpatialDenoiserPass::Execute(ID3D12GraphicsCommandList* cmdList, RenderPassContext& ctx) {
    if (!m_temporalPass) return;
    auto device = ctx.gfx->GetDevice();
    EnsureResources(device, ctx.gfx->GetWidth(), ctx.gfx->GetHeight());

    // Temporal Pass からの 2 系統のクリーンな履歴を受け取る
    auto inputDiffuse = m_temporalPass->GetDenoisedDiffuse();
    auto inputSpecular = m_temporalPass->GetDenoisedSpecular();

    auto normalMap = ctx.gbuffer->GetNormalRoughness();
    auto worldPosMap = ctx.gbuffer->GetWorldPosMetallic();
    auto albedoMap = ctx.gbuffer->GetAlbedo();

    D3D12_RESOURCE_BARRIER preCompute[2] = {
        CD3DX12_RESOURCE_BARRIER::Transition(inputDiffuse, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        CD3DX12_RESOURCE_BARRIER::Transition(inputSpecular, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
    };
    cmdList->ResourceBarrier(2, preCompute);

    ID3D12Resource* srvDiffuse = inputDiffuse;
    ID3D12Resource* srvSpecular = inputSpecular;
    ID3D12Resource* uavDiffuse = m_pingPongDiffuse[0].Get();
    ID3D12Resource* uavSpecular = m_pingPongSpecular[0].Get();

    UINT srvUavSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    CD3DX12_CPU_DESCRIPTOR_HANDLE cpuHandle(m_descriptorHeap->GetCPUDescriptorHandleForHeapStart());
    CD3DX12_GPU_DESCRIPTOR_HANDLE gpuHandle(m_descriptorHeap->GetGPUDescriptorHandleForHeapStart());

    cmdList->SetPipelineState(m_pso.Get());
    cmdList->SetComputeRootSignature(m_rootSig.Get());
    ID3D12DescriptorHeap* heaps[] = { m_descriptorHeap.Get() };
    cmdList->SetDescriptorHeaps(1, heaps);

    // 4 パスの À-Trous ウェーブレットデノイズを実行
    int numPasses = 4;
    for (int i = 0; i < numPasses; ++i) {
        int stepSize = 1 << i; // 1, 2, 4, 8
        bool isLastPass = (i == numPasses - 1);

        // 最後の Pass の場合、書き込み先を m_finalOutput に向ける
        if (isLastPass) {
            uavDiffuse = m_finalOutput.Get();
        }

        // 2 つの UAV をバインド
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

        // uavDiffuse は最後の Pass で UNORM に切り替えてハードウェアに Clamp 変換させる
        uavDesc.Format = isLastPass ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT;
        device->CreateUnorderedAccessView(uavDiffuse, nullptr, &uavDesc, cpuHandle); cpuHandle.Offset(1, srvUavSize);

        uavDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        device->CreateUnorderedAccessView(uavSpecular, nullptr, &uavDesc, cpuHandle);
        auto uavGpuHandle = gpuHandle;
        cpuHandle.Offset(1, srvUavSize); gpuHandle.Offset(2, srvUavSize);

        // 6 つの SRV をバインド
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;
        auto srvGpuHandle = gpuHandle;

        srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; // HDR フォーマットで読み取り
        device->CreateShaderResourceView(srvDiffuse, &srvDesc, cpuHandle); cpuHandle.Offset(1, srvUavSize); gpuHandle.Offset(1, srvUavSize);
        device->CreateShaderResourceView(srvSpecular, &srvDesc, cpuHandle); cpuHandle.Offset(1, srvUavSize); gpuHandle.Offset(1, srvUavSize);

        srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        device->CreateShaderResourceView(normalMap, &srvDesc, cpuHandle); cpuHandle.Offset(1, srvUavSize); gpuHandle.Offset(1, srvUavSize);

        srvDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        device->CreateShaderResourceView(worldPosMap, &srvDesc, cpuHandle); cpuHandle.Offset(1, srvUavSize); gpuHandle.Offset(1, srvUavSize);

        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        device->CreateShaderResourceView(albedoMap, &srvDesc, cpuHandle); cpuHandle.Offset(1, srvUavSize); gpuHandle.Offset(1, srvUavSize);

        srvDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
        device->CreateShaderResourceView(m_temporalPass->GetVarianceOutput(), &srvDesc, cpuHandle); cpuHandle.Offset(1, srvUavSize); gpuHandle.Offset(1, srvUavSize);

        // 5 つの定数を渡す (幅、高さ、ステップ幅、現在の Pass インデックス、最後の Pass かどうか)
        uint32_t constants[5] = { (uint32_t)m_width, (uint32_t)m_height, (uint32_t)stepSize, (uint32_t)i, isLastPass ? 1 : 0 };

        cmdList->SetComputeRoot32BitConstants(0, 5, constants, 0);
        cmdList->SetComputeRootDescriptorTable(1, uavGpuHandle);
        cmdList->SetComputeRootDescriptorTable(2, srvGpuHandle);

        cmdList->Dispatch((m_width + 7) / 8, (m_height + 7) / 8, 1);

        // Ping-Pong 状態切り替えとポインタ交換
        if (i < numPasses - 1) {
            int numBarriers = 0;
            D3D12_RESOURCE_BARRIER bars[4];
            bars[numBarriers++] = CD3DX12_RESOURCE_BARRIER::Transition(uavDiffuse, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            bars[numBarriers++] = CD3DX12_RESOURCE_BARRIER::Transition(uavSpecular, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

            if (srvDiffuse != inputDiffuse) {
                bars[numBarriers++] = CD3DX12_RESOURCE_BARRIER::Transition(srvDiffuse, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                bars[numBarriers++] = CD3DX12_RESOURCE_BARRIER::Transition(srvSpecular, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
            cmdList->ResourceBarrier(numBarriers, bars);

            auto tempDiff = srvDiffuse; srvDiffuse = uavDiffuse; uavDiffuse = (tempDiff == inputDiffuse) ? m_pingPongDiffuse[1].Get() : tempDiff;
            auto tempSpec = srvSpecular; srvSpecular = uavSpecular; uavSpecular = (tempSpec == inputSpecular) ? m_pingPongSpecular[1].Get() : tempSpec;
        }
    }

    // ループ終了後、入力リソースの状態を復元
    D3D12_RESOURCE_BARRIER postCompute[4] = {
        CD3DX12_RESOURCE_BARRIER::Transition(inputDiffuse, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        CD3DX12_RESOURCE_BARRIER::Transition(inputSpecular, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        CD3DX12_RESOURCE_BARRIER::Transition(srvDiffuse, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        CD3DX12_RESOURCE_BARRIER::Transition(srvSpecular, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    };
    cmdList->ResourceBarrier(4, postCompute);

    // finalOutput を SRV に変換し、PostProcessPass が読み取れるようにする
    D3D12_RESOURCE_BARRIER transitionToSRV = CD3DX12_RESOURCE_BARRIER::Transition(
        m_finalOutput.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
    );
    cmdList->ResourceBarrier(1, &transitionToSRV);
}