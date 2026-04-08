#pragma once
#include "pch.h"
#include "GraphicsContext.h"
#include "Scene.h"
#include "GBuffer.h"

// 宣告供各個 Pass 讀取 Shader 的輔助函式 (實作保留於 Renderer.cpp)
std::wstring GetShaderPath(const std::wstring& filename);

// 將渲染所需的共享資源與狀態打包
struct RenderPassContext {
    GraphicsContext* gfx;
    Scene* scene;
    GBuffer* gbuffer;
    ID3D12Resource* lightCB;

    DirectX::XMMATRIX view;
	DirectX::XMMATRIX proj; // 帶有 Jitter，用於實際繪製與 Raytracing 發射光線
	DirectX::XMMATRIX unjitteredProj; // 無 Jitter，用來計算精確的 Velocity
    DirectX::XMVECTOR forward;
    DirectX::XMMATRIX prevView;
    DirectX::XMMATRIX prevProj;
    DirectX::XMMATRIX prevUnjitteredProj; // 上一幀無 Jitter 的投影矩陣

    float jitterX = 0.0f;
    float jitterY = 0.0f;

    int currentDrawCalls = 0;
    int totalVerts = 0;
    int totalPolys = 0;

    // // 目前累計的影格數
    UINT frameCount = 0;
    // --- 帶有雜訊的 Raw GI ---
    ID3D12Resource* rawRaytracingOutput = nullptr;
    // 存放全域相機 Constant Buffer 的 GPU 虛擬位址
    D3D12_GPU_VIRTUAL_ADDRESS passCameraCBAddress = 0;
};

// 抽象 Render Pass 介面
class IRenderPass {
public:
    virtual ~IRenderPass() = default;

    // 初始化 PSO、Root Signature 等專屬資源
    virtual void Init(ID3D12Device* device) = 0;

    // 執行繪製指令
    virtual void Execute(ID3D12GraphicsCommandList* cmdList, RenderPassContext& ctx) = 0;
};