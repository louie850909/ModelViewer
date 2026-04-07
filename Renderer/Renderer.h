#pragma once
#include "pch.h"
#include "GraphicsContext.h"
#include "Scene.h"
#include "GBuffer.h"
#include "IRenderPass.h"
#include "TemporalDenoiserPass.h"
#include <atomic>
#include <chrono>
#include <mutex>
#include <memory>

class Renderer {
public:
    std::mutex m_renderMutex;

    bool Init(IUnknown* panelUnknown, int width, int height);
    void Resize(int width, int height, float scale);
    void RenderFrame();
    void Shutdown();

    int AddMesh(std::shared_ptr<Mesh> mesh) { return m_scene.AddMesh(mesh); }
    void RemoveMeshById(int meshId);
    void UploadMeshToGpu(std::shared_ptr<Mesh> mesh, int meshId);

    void SetCameraTransform(float px, float py, float pz, float pitch, float yaw) { m_scene.SetCameraTransform(px, py, pz, pitch, yaw); }
    void GetStats(int& vertices, int& polygons, int& drawCalls, float& frameTimeMs);

    // Node API & Light API 委派給 Scene
    int GetTotalNodeCount() { return m_scene.GetTotalNodeCount(); }
    bool GetNodeInfo(int globalIndex, std::string& outName, int& outParentGlobal) { return m_scene.GetNodeInfo(globalIndex, outName, outParentGlobal); }
    bool GetNodeTransform(int globalIndex, float* outT, float* outR, float* outS) { return m_scene.GetNodeTransform(globalIndex, outT, outR, outS); }
    bool SetNodeTransform(int globalIndex, const float* inT, const float* inR, const float* inS) { return m_scene.SetNodeTransform(globalIndex, inT, inR, inS); }
    std::shared_ptr<Mesh> GetMesh() const { return m_scene.GetMesh(); }

    int AddLight(int type) { return m_scene.AddLight(type); }
    void RemoveLight(int id) { m_scene.RemoveLight(id); }
    LightNode* GetLight(int id) { return m_scene.GetLight(id); }
    const std::vector<LightNode>& GetLights() const { return m_scene.GetLights(); }

    void SetRayTracingEnabled(bool enable) { m_rayTracingEnabled = enable; }

private:
    UINT m_frameCount = 0;
    DirectX::XMMATRIX m_prevView = DirectX::XMMatrixIdentity();
    DirectX::XMMATRIX m_prevProj = DirectX::XMMatrixIdentity();
    void UpdateLightBuffer();

    bool m_rayTracingEnabled = false;

    GraphicsContext m_ctx;
    Scene m_scene;
    GBuffer m_gBuffer;

    struct LightBufferData {
        int numLights;
        DirectX::XMFLOAT3 cameraPos;
        struct Light {
            int type; float intensity; float coneAngle; float _pad1;
            float color[3]; float _pad2;
            float position[3]; float _pad3;
            float direction[3]; float _pad4;
        } lights[16];
    };
    ComPtr<ID3D12Resource> m_lightCB;
    LightBufferData* m_mappedLightCB = nullptr;

    // --- 管線模組化 ---
    std::unique_ptr<IRenderPass> m_geomPass;
    std::unique_ptr<IRenderPass> m_lightPass;
    std::unique_ptr<IRenderPass> m_transparentPass;
    std::unique_ptr<IRenderPass> m_rayTracingPass;
    std::unique_ptr<IRenderPass> m_temporalDenoiserPass;

    std::atomic<bool> m_isShuttingDown{ false };
    std::atomic<int>   m_statVertices{ 0 };
    std::atomic<int>   m_statPolygons{ 0 };
    std::atomic<int>   m_statDrawCalls{ 0 };
    std::atomic<float> m_statFrameTime{ 0.0f };
    std::chrono::high_resolution_clock::time_point m_lastFrameTime;
};