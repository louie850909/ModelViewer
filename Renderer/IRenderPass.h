#pragma once
#include "pch.h"
#include "GraphicsContext.h"
#include "Scene.h"
#include "GBuffer.h"

// 各 Pass が Shader を読み取るためのヘルパー関数を宣言 (実装は Renderer.cpp に保持)
std::wstring GetShaderPath(const std::wstring& filename);

// レンダリングに必要な共有リソースと状態をパッケージ化
struct RenderPassContext {
    GraphicsContext* gfx;
    Scene* scene;
    GBuffer* gbuffer;
    ID3D12Resource* lightCB;

    DirectX::XMMATRIX view;
	DirectX::XMMATRIX proj; // Jitter あり、実際の描画と Raytracing の光線発射に使用
	DirectX::XMMATRIX unjitteredProj; // Jitter なし、正確な Velocity の計算に使用
    DirectX::XMVECTOR forward;
    DirectX::XMMATRIX prevView;
    DirectX::XMMATRIX prevProj;
    DirectX::XMMATRIX prevUnjitteredProj; // 前フレームの Jitter なし投影行列

    float jitterX = 0.0f;
    float jitterY = 0.0f;

    int currentDrawCalls = 0;
    int totalVerts = 0;
    int totalPolys = 0;

    // // 現在累積されたフレーム数
    UINT frameCount = 0;
    // --- ノイズありの Raw GI ---
    ID3D12Resource* rawDiffuseGI = nullptr;
    ID3D12Resource* rawSpecularGI = nullptr;
    // グローバルカメラ Constant Buffer の GPU 仮想アドレスを格納
    D3D12_GPU_VIRTUAL_ADDRESS passCameraCBAddress = 0;
};

// 抽象 Render Pass インターフェース
class IRenderPass {
public:
    virtual ~IRenderPass() = default;

    // PSO、Root Signature などの専用リソースを初期化
    virtual void Init(ID3D12Device* device) = 0;

    // 描画命令を実行
    virtual void Execute(ID3D12GraphicsCommandList* cmdList, RenderPassContext& ctx) = 0;
};