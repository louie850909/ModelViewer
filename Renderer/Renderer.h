#pragma once
#include "pch.h"
#include "GraphicsContext.h"
#include "Scene.h"
#include <atomic>
#include <chrono>
#include <mutex>

class Renderer {
public:
    std::mutex m_renderMutex;

    bool Init(IUnknown* panelUnknown, int width, int height);
    void Resize(int width, int height, float scale);
    void RenderFrame();
    void Shutdown();

    int AddMesh(std::shared_ptr<Mesh> mesh) { return m_scene.AddMesh(mesh); }
    void RemoveMeshById(int meshId) {
        std::lock_guard<std::mutex> lock(m_renderMutex);
        m_ctx.WaitForGpu();
        m_scene.RemoveMeshById(meshId);
    }

    void UploadMeshToGpu(std::shared_ptr<Mesh> mesh, int meshId);

    void SetCameraTransform(float px, float py, float pz, float pitch, float yaw) { m_scene.SetCameraTransform(px, py, pz, pitch, yaw); }
    void GetStats(int& vertices, int& polygons, int& drawCalls, float& frameTimeMs);

    // Node API 委派給 Scene
    int GetTotalNodeCount() { return m_scene.GetTotalNodeCount(); }
    bool GetNodeInfo(int globalIndex, std::string& outName, int& outParentGlobal) { return m_scene.GetNodeInfo(globalIndex, outName, outParentGlobal); }
    bool GetNodeTransform(int globalIndex, float* outT, float* outR, float* outS) { return m_scene.GetNodeTransform(globalIndex, outT, outR, outS); }
    bool SetNodeTransform(int globalIndex, const float* inT, const float* inR, const float* inS) { return m_scene.SetNodeTransform(globalIndex, inT, inR, inS); }
    std::shared_ptr<Mesh> GetMesh() const { return m_scene.GetMesh(); }

private:
    void CreateRootSignatureAndPSO();

    GraphicsContext m_ctx;
    Scene m_scene;

    ComPtr<ID3D12RootSignature> m_rootSig;
    ComPtr<ID3D12PipelineState> m_psoOpaque;
    ComPtr<ID3D12PipelineState> m_psoTransparent;
    UINT m_srvDescriptorSize = 0;

    std::atomic<int>   m_statVertices{ 0 };
    std::atomic<int>   m_statPolygons{ 0 };
    std::atomic<int>   m_statDrawCalls{ 0 };
    std::atomic<float> m_statFrameTime{ 0.0f };
    std::chrono::high_resolution_clock::time_point m_lastFrameTime;
};