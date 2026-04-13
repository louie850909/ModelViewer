#pragma once
#include "IRenderPass.h"

class PostProcessPass : public IRenderPass {
public:
    void Init(ID3D12Device* device) override;
    void Execute(ID3D12GraphicsCommandList* cmdList, RenderPassContext& ctx) override;

    void SetSharpness(float sharpness) { m_sharpness = sharpness; }

private:
    void EnsureResources(ID3D12Device* device, int width, int height);

    ComPtr<ID3D12RootSignature> m_rootSig;
    ComPtr<ID3D12PipelineState> m_pso;
    ComPtr<ID3D12DescriptorHeap> m_srvUavHeap;

    // 因為 BackBuffer 不支援 UAV，我們需要一個中繼資源
    ComPtr<ID3D12Resource> m_uavOutput;

    float m_sharpness = 0.5f;
    int m_width = 0;
    int m_height = 0;
};