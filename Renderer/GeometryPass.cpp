#include "pch.h"
#include "GeometryPass.h"

void GeometryPass::Init(ID3D12Device* device) {
    CD3DX12_DESCRIPTOR_RANGE1 geomSrvRange;
    geomSrvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0);
    CD3DX12_ROOT_PARAMETER1 geomParams[3];
    geomParams[0].InitAsConstantBufferView(0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_NONE, D3D12_SHADER_VISIBILITY_VERTEX); // b0: グローバルカメラ (2 DWORDs)
    geomParams[1].InitAsConstants(16, 1, 0, D3D12_SHADER_VISIBILITY_VERTEX);                                       // b1: Model Matrix (16 DWORDs)
    geomParams[2].InitAsDescriptorTable(1, &geomSrvRange, D3D12_SHADER_VISIBILITY_PIXEL);                          // t0: Textures (1 DWORD)

    CD3DX12_STATIC_SAMPLER_DESC sampler(0, D3D12_FILTER_ANISOTROPIC);
    sampler.MaxAnisotropy = 16;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC geomRsDesc;
    geomRsDesc.Init_1_1(3, geomParams, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
    ComPtr<ID3DBlob> sigBlob, errBlob;
    D3DX12SerializeVersionedRootSignature(&geomRsDesc, D3D_ROOT_SIGNATURE_VERSION_1_1, &sigBlob, &errBlob);
    device->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&m_rootSig));

    ComPtr<ID3DBlob> geomVS, geomPS;
    D3DReadFileToBlob(GetShaderPath(L"GBuffer_VS.cso").c_str(), &geomVS);
    D3DReadFileToBlob(GetShaderPath(L"GBuffer_PS.cso").c_str(), &geomPS);

    D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC geomPsoDesc = {};
    geomPsoDesc.InputLayout = { layout, _countof(layout) };
    geomPsoDesc.pRootSignature = m_rootSig.Get();
    geomPsoDesc.VS = CD3DX12_SHADER_BYTECODE(geomVS.Get());
    geomPsoDesc.PS = CD3DX12_SHADER_BYTECODE(geomPS.Get());
    geomPsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    geomPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    geomPsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    geomPsoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    geomPsoDesc.SampleMask = UINT_MAX;
    geomPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    geomPsoDesc.NumRenderTargets = 4;
    geomPsoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    geomPsoDesc.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    geomPsoDesc.RTVFormats[2] = DXGI_FORMAT_R32G32B32A32_FLOAT;
    geomPsoDesc.RTVFormats[3] = DXGI_FORMAT_R16G16_FLOAT;
    geomPsoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    geomPsoDesc.SampleDesc.Count = 1;
    device->CreateGraphicsPipelineState(&geomPsoDesc, IID_PPV_ARGS(&m_pso));
}

void GeometryPass::Execute(ID3D12GraphicsCommandList* cmdList, RenderPassContext& ctx) {
    // == GBuffer を SRV から RTV に変換 ==
    D3D12_RESOURCE_BARRIER barriersToRTV[4] = {
        CD3DX12_RESOURCE_BARRIER::Transition(ctx.gbuffer->GetAlbedo(),   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
        CD3DX12_RESOURCE_BARRIER::Transition(ctx.gbuffer->GetNormalRoughness(),   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
        CD3DX12_RESOURCE_BARRIER::Transition(ctx.gbuffer->GetWorldPosMetallic(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
        CD3DX12_RESOURCE_BARRIER::Transition(ctx.gbuffer->GetVelocity(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET)
    };
    cmdList->ResourceBarrier(4, barriersToRTV);

    D3D12_CPU_DESCRIPTOR_HANDLE gbufferRTVs = ctx.gbuffer->GetRtvStart();
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = ctx.gfx->GetDSV();
    cmdList->OMSetRenderTargets(4, &gbufferRTVs, TRUE, &dsv);

    float clearGBuffer[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(gbufferRTVs);
    auto device = ctx.gfx->GetDevice();
    for (int i = 0; i < 4; ++i) {
        cmdList->ClearRenderTargetView(rtvHandle, clearGBuffer, 0, nullptr);
        rtvHandle.Offset(1, device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV));
    }
    cmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    cmdList->SetGraphicsRootSignature(m_rootSig.Get());
    cmdList->SetGraphicsRootConstantBufferView(0, ctx.passCameraCBAddress);
    cmdList->SetPipelineState(m_pso.Get());
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    using namespace DirectX;
    UINT srvDescSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    for (auto& inst : ctx.scene->GetMeshes()) {
        auto& mesh = inst.mesh;
        if (!mesh) continue;

        std::vector<XMMATRIX> globalTransforms(mesh->nodes.size());
        for (size_t i = 0; i < mesh->nodes.size(); ++i) {
            const auto& node = mesh->nodes[i];
            XMMATRIX local = XMMatrixScaling(node.s[0], node.s[1], node.s[2]) * XMMatrixRotationQuaternion(XMVectorSet(node.r[0], node.r[1], node.r[2], node.r[3])) * XMMatrixTranslation(node.t[0], node.t[1], node.t[2]);
            globalTransforms[i] = (node.parentIndex >= 0) ? local * globalTransforms[node.parentIndex] : local;
        }

        cmdList->IASetVertexBuffers(0, 1, &mesh->vbView);
        cmdList->IASetIndexBuffer(&mesh->ibView);
        ID3D12DescriptorHeap* heaps[] = { inst.srvHeap.Get() };
        cmdList->SetDescriptorHeaps(1, heaps);

        for (size_t n = 0; n < mesh->nodes.size(); ++n) {
            const auto& node = mesh->nodes[n];
            if (node.subMeshIndices.empty()) continue;

            XMMATRIX modelMat = globalTransforms[n];
            XMFLOAT4X4 modelFloat4x4;
            XMStoreFloat4x4(&modelFloat4x4, XMMatrixTranspose(modelMat));
            cmdList->SetGraphicsRoot32BitConstants(1, 16, &modelFloat4x4, 0); // ループ内で 16 個の float のみを送信

            for (int subIdx : node.subMeshIndices) {
                const auto& sub = mesh->subMeshes[subIdx];
                if (sub.isTransparent) continue;

                int matIdx = (sub.materialIndex >= 0 && sub.materialIndex < (int)mesh->texturePaths.size()) ? sub.materialIndex : 0;
                CD3DX12_GPU_DESCRIPTOR_HANDLE srvHandle(inst.srvHeap->GetGPUDescriptorHandleForHeapStart(), matIdx * 3, srvDescSize);
                cmdList->SetGraphicsRootDescriptorTable(2, srvHandle);
                cmdList->DrawIndexedInstanced(sub.indexCount, 1, sub.indexOffset, 0, 0);
                ctx.currentDrawCalls++;
            }
        }
        ctx.totalVerts += (int)mesh->vertices.size();
        ctx.totalPolys += (int)mesh->indices.size() / 3;
    }

    D3D12_RESOURCE_BARRIER barriersToSRV[4] = {
        CD3DX12_RESOURCE_BARRIER::Transition(ctx.gbuffer->GetAlbedo(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        CD3DX12_RESOURCE_BARRIER::Transition(ctx.gbuffer->GetNormalRoughness(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        CD3DX12_RESOURCE_BARRIER::Transition(ctx.gbuffer->GetWorldPosMetallic(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        CD3DX12_RESOURCE_BARRIER::Transition(ctx.gbuffer->GetVelocity(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
    };
    cmdList->ResourceBarrier(4, barriersToSRV);
}