#pragma once
#include "IRenderPass.h"

class TemporalDenoiserPass : public IRenderPass {
public:
    void Init(ID3D12Device* device) override;
    void Execute(ID3D12GraphicsCommandList* cmdList, RenderPassContext& ctx) override;

    ID3D12Resource* GetDenoisedOutput() const { return m_history[m_writeIdx].Get(); }

private:
    void EnsureResources(ID3D12Device* device, int width, int height);

    ComPtr<ID3D12RootSignature> m_rootSig;
    ComPtr<ID3D12PipelineState> m_pso;

    // 兩張 Ping-Pong Buffer 互換讀寫
    ComPtr<ID3D12Resource> m_history[2];
    ComPtr<ID3D12DescriptorHeap> m_descriptorHeap;
    ComPtr<ID3D12DescriptorHeap> m_clearHeapForInit;
    bool m_needsClear = true;

    int m_width = 0;
    int m_height = 0;
    int m_writeIdx = 0;
};
