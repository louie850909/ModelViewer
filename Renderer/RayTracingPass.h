#pragma once
#include "IRenderPass.h"

class RayTracingPass : public IRenderPass {
public:
    void Init(ID3D12Device* device) override;
    void Execute(ID3D12GraphicsCommandList* cmdList, RenderPassContext& ctx) override;

private:
    void BuildTLAS(ID3D12GraphicsCommandList4* cmdList4, RenderPassContext& ctx);
    void EnsureOutputTexture(ID3D12Device* device, int width, int height);

    // TLAS 相關資源
    ComPtr<ID3D12Resource> m_tlasBuffer;
    ComPtr<ID3D12Resource> m_tlasScratch;
    ComPtr<ID3D12Resource> m_instanceDescBuffer;
    UINT m_maxInstances = 1000; // 預留最大物件數量

    // DXR 輸出資源
    ComPtr<ID3D12Resource> m_raytracingOutput;
    int m_outputWidth = 0;
    int m_outputHeight = 0;
};
