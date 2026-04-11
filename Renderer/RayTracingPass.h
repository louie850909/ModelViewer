#pragma once
#include "IRenderPass.h"

class RayTracingPass : public IRenderPass {
public:
    void Init(ID3D12Device* device) override;
    void Execute(ID3D12GraphicsCommandList* cmdList, RenderPassContext& ctx) override;

    ID3D12Resource* GetDiffuseOutput() const { return m_diffuseOutput.Get(); }
    ID3D12Resource* GetSpecularOutput() const { return m_specularOutput.Get(); }

private:
    void BuildTLAS(ID3D12GraphicsCommandList4* cmdList4, RenderPassContext& ctx);
    void EnsureOutputTexture(ID3D12Device* device, int width, int height);

    // TLAS 相關資源
    ComPtr<ID3D12Resource> m_tlasBuffer;
    ComPtr<ID3D12Resource> m_tlasScratch;
    ComPtr<ID3D12Resource> m_instanceDescBuffer;
    UINT m_maxInstances = 1000; // 預留最大物件數量

    // DXR 輸出資源
    ComPtr<ID3D12Resource> m_diffuseOutput;
    ComPtr<ID3D12Resource> m_specularOutput;
    int m_outputWidth = 0;
    int m_outputHeight = 0;

    // DXR 核心資源
    ComPtr<ID3D12RootSignature> m_globalRootSig;
    ComPtr<ID3D12StateObject>   m_dxrStateObject;
    ComPtr<ID3D12Resource>      m_sbtBuffer;
    ComPtr<ID3D12DescriptorHeap> m_descriptorHeap; // 供 UAV 使用

    // 相機常數緩衝區
    ComPtr<ID3D12Resource> m_cameraCB;
    struct CameraParams {
        DirectX::XMFLOAT4X4 viewProjInv;
        DirectX::XMFLOAT3 cameraPos;
        UINT frameCount;
    };
    CameraParams* m_mappedCameraCB = nullptr;

	// SBT 相關參數
    ComPtr<ID3D12RootSignature> m_localRootSig;
    UINT m_instanceCount = 0;
    UINT m_sbtHitGroupOffset = 0;
    UINT m_sbtHitGroupStride = 0;

    void CreateRootSignature(ID3D12Device5* device);
    void CreateLocalRootSignature(ID3D12Device5* device);
    void CreatePipelineState(ID3D12Device5* device);
    void BuildSBT(ID3D12Device5* device, RenderPassContext& ctx);
};
