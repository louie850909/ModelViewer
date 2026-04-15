#pragma once
#include "IRenderPass.h"
#include "HDRILoader.h"

class RayTracingPass : public IRenderPass {
public:
    void Init(ID3D12Device* device) override;
    void Execute(ID3D12GraphicsCommandList* cmdList, RenderPassContext& ctx) override;

    ID3D12Resource* GetDiffuseOutput() const { return m_diffuseOutput.Get(); }
    ID3D12Resource* GetSpecularOutput() const { return m_specularOutput.Get(); }

    void SetEnvironmentMap(std::shared_ptr<HDRIResource> envMap) {
        m_envMap = envMap;
        m_envMapDirty = true; // 環境マップが更新されたことをマーク
    }

private:
    void BuildTLAS(ID3D12GraphicsCommandList4* cmdList4, RenderPassContext& ctx);
    void EnsureOutputTexture(ID3D12Device* device, int width, int height);

    // TLAS 関連リソース
    ComPtr<ID3D12Resource> m_tlasBuffer;
    ComPtr<ID3D12Resource> m_tlasScratch;
    ComPtr<ID3D12Resource> m_instanceDescBuffer;
    UINT m_maxInstances = 1000; // 最大オブジェクト数を予約

    // DXR 出力リソース
    ComPtr<ID3D12Resource> m_diffuseOutput;
    ComPtr<ID3D12Resource> m_specularOutput;
    int m_outputWidth = 0;
    int m_outputHeight = 0;

    // DXR コアリソース
    ComPtr<ID3D12RootSignature> m_globalRootSig;
    ComPtr<ID3D12StateObject>   m_dxrStateObject;
    ComPtr<ID3D12Resource>      m_sbtBuffer;
    ComPtr<ID3D12DescriptorHeap> m_descriptorHeap; // UAV 用

	// 環境マップリソース
    std::shared_ptr<HDRIResource> m_envMap;
    // 初期化時の一時バッファ、後で解放可能
    ComPtr<ID3D12Resource> m_envMapUpload;

    // カメラ定数バッファ
    ComPtr<ID3D12Resource> m_cameraCB;
    struct CameraParams {
        DirectX::XMFLOAT4X4 viewProjInv;
        DirectX::XMFLOAT3 cameraPos;
        UINT frameCount;
		float envIntegral;// 環境光の総エネルギー
        float jitterX;    // NDC 空間の jitter オフセット (透過ピクセルで無効化するため)
        float jitterY;
        float _pad;
    };
    CameraParams* m_mappedCameraCB = nullptr;

	// SBT 関連パラメータ
    ComPtr<ID3D12RootSignature> m_localRootSig;
    UINT m_instanceCount = 0;
    UINT m_sbtHitGroupOffset = 0;
    UINT m_sbtHitGroupStride = 0;

    // 状態追跡変数
    uint32_t m_lastStructureRevision = 0;
    uint32_t m_lastTransformRevision = 0;
    bool m_envMapDirty = true;

    void CreateRootSignature(ID3D12Device5* device);
    void CreateLocalRootSignature(ID3D12Device5* device);
    void CreatePipelineState(ID3D12Device5* device);
    void BuildSBT(ID3D12Device5* device, RenderPassContext& ctx);
};
