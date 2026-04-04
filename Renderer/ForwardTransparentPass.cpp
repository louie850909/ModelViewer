#include "pch.h"
#include "ForwardTransparentPass.h"

void ForwardTransparentPass::Init(ID3D12Device* device) {
    CD3DX12_DESCRIPTOR_RANGE1 fwdSrvRange;
    fwdSrvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0);
    CD3DX12_ROOT_PARAMETER1 fwdParams[3];
    fwdParams[0].InitAsConstants(sizeof(SceneConstants) / 4, 0);
    fwdParams[1].InitAsDescriptorTable(1, &fwdSrvRange, D3D12_SHADER_VISIBILITY_PIXEL);
    fwdParams[2].InitAsConstantBufferView(1, 0, D3D12_ROOT_DESCRIPTOR_FLAG_NONE, D3D12_SHADER_VISIBILITY_PIXEL);

    CD3DX12_STATIC_SAMPLER_DESC sampler(0, D3D12_FILTER_ANISOTROPIC);
    sampler.MaxAnisotropy = 16;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC fwdRsDesc;
    fwdRsDesc.Init_1_1(3, fwdParams, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    ComPtr<ID3DBlob> sigBlob, errBlob;
    D3DX12SerializeVersionedRootSignature(&fwdRsDesc, D3D_ROOT_SIGNATURE_VERSION_1_1, &sigBlob, &errBlob);
    device->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&m_rootSig));

    ComPtr<ID3DBlob> fwdVS, fwdPS;
    D3DReadFileToBlob(GetShaderPath(L"ForwardTransparent_VS.cso").c_str(), &fwdVS);
    D3DReadFileToBlob(GetShaderPath(L"ForwardTransparent_PS.cso").c_str(), &fwdPS);

    D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC transPsoDesc = {};
    transPsoDesc.InputLayout = { layout, _countof(layout) };
    transPsoDesc.pRootSignature = m_rootSig.Get();
    transPsoDesc.VS = CD3DX12_SHADER_BYTECODE(fwdVS.Get());
    transPsoDesc.PS = CD3DX12_SHADER_BYTECODE(fwdPS.Get());
    transPsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    transPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    transPsoDesc.SampleMask = UINT_MAX;
    transPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    transPsoDesc.NumRenderTargets = 1;
    transPsoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    transPsoDesc.SampleDesc.Count = 1;

    D3D12_RENDER_TARGET_BLEND_DESC blendDesc = {};
    blendDesc.BlendEnable = TRUE;
    blendDesc.LogicOpEnable = FALSE;
    blendDesc.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    blendDesc.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    blendDesc.BlendOp = D3D12_BLEND_OP_ADD;
    blendDesc.SrcBlendAlpha = D3D12_BLEND_ONE;
    blendDesc.DestBlendAlpha = D3D12_BLEND_ZERO;
    blendDesc.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blendDesc.LogicOp = D3D12_LOGIC_OP_NOOP;
    blendDesc.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    transPsoDesc.BlendState.RenderTarget[0] = blendDesc;

    transPsoDesc.DepthStencilState.DepthEnable = TRUE;
    transPsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    transPsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;

    device->CreateGraphicsPipelineState(&transPsoDesc, IID_PPV_ARGS(&m_pso));
}

void ForwardTransparentPass::Execute(ID3D12GraphicsCommandList* cmdList, RenderPassContext& ctx) {
    using namespace DirectX;
    D3D12_CPU_DESCRIPTOR_HANDLE backBufferRTV = ctx.gfx->GetCurrentBackBufferRTV();
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = ctx.gfx->GetDSV();
    cmdList->OMSetRenderTargets(1, &backBufferRTV, FALSE, &dsv);

    cmdList->SetGraphicsRootSignature(m_rootSig.Get());
    cmdList->SetGraphicsRootConstantBufferView(2, ctx.lightCB->GetGPUVirtualAddress());
    cmdList->SetPipelineState(m_pso.Get());

    auto device = ctx.gfx->GetDevice();
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
            SceneConstants cb = {};
            XMStoreFloat4x4(&cb.mvp, XMMatrixTranspose(modelMat * ctx.view * ctx.proj));
            XMStoreFloat4x4(&cb.modelMatrix, XMMatrixTranspose(modelMat));
            XMStoreFloat4x4(&cb.normalMatrix, XMMatrixInverse(nullptr, modelMat));
            XMStoreFloat3(&cb.lightDir, XMVector3Normalize(ctx.forward));
            cb.cameraPos = ctx.scene->GetCameraPos();
            cmdList->SetGraphicsRoot32BitConstants(0, sizeof(SceneConstants) / 4, &cb, 0);

            for (int subIdx : node.subMeshIndices) {
                const auto& sub = mesh->subMeshes[subIdx];
                if (!sub.isTransparent) continue;

                int matIdx = (sub.materialIndex >= 0 && sub.materialIndex < (int)mesh->texturePaths.size()) ? sub.materialIndex : 0;
                CD3DX12_GPU_DESCRIPTOR_HANDLE srvHandle(inst.srvHeap->GetGPUDescriptorHandleForHeapStart(), matIdx * 2, srvDescSize);
                cmdList->SetGraphicsRootDescriptorTable(1, srvHandle);
                cmdList->DrawIndexedInstanced(sub.indexCount, 1, sub.indexOffset, 0, 0);
                ctx.currentDrawCalls++;
            }
        }
    }
}