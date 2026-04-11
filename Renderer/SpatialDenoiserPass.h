#pragma once
#include "IRenderPass.h"
#include "TemporalDenoiserPass.h"

class SpatialDenoiserPass : public IRenderPass {
public:
    void Init(ID3D12Device* device) override;
    void Execute(ID3D12GraphicsCommandList* cmdList, RenderPassContext& ctx) override;

    void SetTemporalPass(TemporalDenoiserPass* tempPass) { m_temporalPass = tempPass; }

private:
    void EnsureResources(ID3D12Device* device, int width, int height);

    ComPtr<ID3D12RootSignature> m_rootSig;
    ComPtr<ID3D12PipelineState> m_pso;

    // 擴充為雙軌 Ping-Pong 緩衝區
    ComPtr<ID3D12Resource> m_pingPongDiffuse[2];
    ComPtr<ID3D12Resource> m_pingPongSpecular[2];

    ComPtr<ID3D12DescriptorHeap> m_descriptorHeap;

    TemporalDenoiserPass* m_temporalPass = nullptr;

    int m_width = 0;
    int m_height = 0;
};