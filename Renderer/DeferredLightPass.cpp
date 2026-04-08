#include "pch.h"
#include "DeferredLightPass.h"

void DeferredLightPass::Init(ID3D12Device* device) {
    CD3DX12_DESCRIPTOR_RANGE1 lightSrvRange;
    lightSrvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0);
    CD3DX12_ROOT_PARAMETER1 lightParams[3];
    lightParams[0].InitAsConstants(sizeof(SceneConstants) / 4, 0);
    lightParams[1].InitAsDescriptorTable(1, &lightSrvRange, D3D12_SHADER_VISIBILITY_PIXEL);
    lightParams[2].InitAsConstantBufferView(1, 0, D3D12_ROOT_DESCRIPTOR_FLAG_NONE, D3D12_SHADER_VISIBILITY_PIXEL);

    CD3DX12_STATIC_SAMPLER_DESC lightSampler(0, D3D12_FILTER_MIN_MAG_MIP_POINT);
    lightSampler.AddressU = lightSampler.AddressV = lightSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC lightRsDesc;
    lightRsDesc.Init_1_1(3, lightParams, 1, &lightSampler, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    ComPtr<ID3DBlob> sigBlob, errBlob;
    D3DX12SerializeVersionedRootSignature(&lightRsDesc, D3D_ROOT_SIGNATURE_VERSION_1_1, &sigBlob, &errBlob);
    device->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&m_rootSig));

    ComPtr<ID3DBlob> lightVS, lightPS;
    D3DReadFileToBlob(GetShaderPath(L"DeferredLight_VS.cso").c_str(), &lightVS);
    D3DReadFileToBlob(GetShaderPath(L"DeferredLight_PS.cso").c_str(), &lightPS);

    D3D12_GRAPHICS_PIPELINE_STATE_DESC lightPsoDesc = {};
    lightPsoDesc.pRootSignature = m_rootSig.Get();
    lightPsoDesc.VS = CD3DX12_SHADER_BYTECODE(lightVS.Get());
    lightPsoDesc.PS = CD3DX12_SHADER_BYTECODE(lightPS.Get());
    lightPsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    lightPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    lightPsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    lightPsoDesc.DepthStencilState.DepthEnable = FALSE;
    lightPsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    lightPsoDesc.SampleMask = UINT_MAX;
    lightPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    lightPsoDesc.NumRenderTargets = 1;
    lightPsoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    lightPsoDesc.SampleDesc.Count = 1;
    device->CreateGraphicsPipelineState(&lightPsoDesc, IID_PPV_ARGS(&m_pso));
}

void DeferredLightPass::Execute(ID3D12GraphicsCommandList* cmdList, RenderPassContext& ctx) {
    using namespace DirectX;
    D3D12_CPU_DESCRIPTOR_HANDLE backBufferRTV = ctx.gfx->GetCurrentBackBufferRTV();
    cmdList->OMSetRenderTargets(1, &backBufferRTV, FALSE, nullptr);

    cmdList->SetGraphicsRootSignature(m_rootSig.Get());
    cmdList->SetPipelineState(m_pso.Get());

    ID3D12DescriptorHeap* gbufferHeaps[] = { ctx.gbuffer->GetSrvHeap() };
    cmdList->SetDescriptorHeaps(1, gbufferHeaps);

    SceneConstants lightCb = {};
    cmdList->SetGraphicsRoot32BitConstants(0, sizeof(SceneConstants) / 4, &lightCb, 0);
    cmdList->SetGraphicsRootConstantBufferView(2, ctx.lightCB->GetGPUVirtualAddress());
    cmdList->SetGraphicsRootDescriptorTable(1, ctx.gbuffer->GetSrvStart());

    cmdList->DrawInstanced(3, 1, 0, 0);
    ctx.currentDrawCalls++;
}