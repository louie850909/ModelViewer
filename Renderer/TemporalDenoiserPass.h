#pragma once
#include "IRenderPass.h"

class TemporalDenoiserPass : public IRenderPass {
public:
    void Init(ID3D12Device* device) override;
    void Execute(ID3D12GraphicsCommandList* cmdList, RenderPassContext& ctx) override;

    // 將回傳的結果指向 m_historyGI
    ID3D12Resource* GetDenoisedOutput() const { return m_historyGI[m_writeIdx].Get(); }

private:
    void EnsureResources(ID3D12Device* device, int width, int height);

    ComPtr<ID3D12RootSignature> m_rootSig;
    ComPtr<ID3D12PipelineState> m_pso;

    // SVGF 需要儲存的三種歷史資訊 (Ping-Pong 互換讀寫)
    ComPtr<ID3D12Resource> m_historyGI[2];
    ComPtr<ID3D12Resource> m_historyNormal[2];
    ComPtr<ID3D12Resource> m_historyPos[2];

    ComPtr<ID3D12DescriptorHeap> m_descriptorHeap;

    int m_width = 0;
    int m_height = 0;
    int m_writeIdx = 0;
};