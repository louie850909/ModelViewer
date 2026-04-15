#pragma once
#include "IRenderPass.h"

class TemporalDenoiserPass : public IRenderPass {
public:
    void Init(ID3D12Device* device) override;
    void Execute(ID3D12GraphicsCommandList* cmdList, RenderPassContext& ctx) override;

    // 返却結果を m_historyGI に向ける
    ID3D12Resource* GetDenoisedDiffuse() const { return m_historyDiffuse[m_writeIdx].Get(); }
    ID3D12Resource* GetDenoisedSpecular() const { return m_historySpecular[m_writeIdx].Get(); }
	// Variance Output (Spatial Pass が読み取るため)
    ID3D12Resource* GetVarianceOutput() const { return m_varianceOutput.Get(); }

private:
    struct TemporalConstants
    {
        uint32_t width;
        uint32_t height;
        DirectX::XMFLOAT3 cameraPos;
        float _pad;
        DirectX::XMFLOAT4X4 prevViewProj;
    };

    void EnsureResources(ID3D12Device* device, int width, int height);

    ComPtr<ID3D12RootSignature> m_rootSig;
    ComPtr<ID3D12PipelineState> m_pso;

    // SVGF が保存する必要がある 3 種類の履歴情報 (Ping-Pong で読み書き切り替え)
    ComPtr<ID3D12Resource> m_historyDiffuse[2];
    ComPtr<ID3D12Resource> m_historySpecular[2];
    ComPtr<ID3D12Resource> m_historyNormal[2];
    ComPtr<ID3D12Resource> m_historyPos[2];
    ComPtr<ID3D12Resource> m_varianceOutput;
    ComPtr<ID3D12DescriptorHeap> m_descriptorHeap;

    int m_width = 0;
    int m_height = 0;
    int m_writeIdx = 0;
};