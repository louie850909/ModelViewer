#include "pch.h"
#include "Renderer.h"
#include "MeshLoader.h"
#include <thread>
#include <atomic>

static Renderer           g_renderer;
static std::thread        g_renderThread;
static std::atomic<bool>  g_running{ false };
static std::atomic<bool>  g_resizePending{ false };
static std::atomic<int>   g_newW{ 0 };
static std::atomic<int>   g_newH{ 0 };
static std::atomic<float> g_newScale{ 1.0f };

extern "C" {

    typedef void (*LoadCallback)(int meshId);

    // ---------------------------------------------------------------------------
    // 生命週期
    // ---------------------------------------------------------------------------
    __declspec(dllexport) bool Renderer_Init(IUnknown* panelUnknown, int width, int height) {
        if (!g_renderer.Init(panelUnknown, width, height)) return false;
        g_running = true;
        g_renderThread = std::thread([]() {
            while (g_running) {
                if (g_resizePending.exchange(false))
                    g_renderer.Resize(g_newW, g_newH, g_newScale);
                g_renderer.RenderFrame();
            }
            });
        return true;
    }

    __declspec(dllexport) void Renderer_Resize(int width, int height, float scale) {
        g_newW.store(width); g_newH.store(height); g_newScale.store(scale);
        g_resizePending.store(true);
    }

    __declspec(dllexport) void Renderer_Shutdown() {
        g_running = false;
        if (g_renderThread.joinable()) g_renderThread.join();
        g_renderer.Shutdown();
    }

    // ---------------------------------------------------------------------------
    // 模型載入 / 移除
    // ---------------------------------------------------------------------------
    __declspec(dllexport) int Renderer_AddModel(const char* path, LoadCallback callback) {
        std::string filePath(path);
        int meshId = g_renderer.AddMesh(nullptr);
        std::thread([filePath, callback, meshId]() {
            auto mesh = MeshLoader::Load(filePath);
            if (mesh)
                g_renderer.UploadMeshToGpu(mesh, meshId);
            if (callback) callback(meshId);
            }).detach();
        return meshId;
    }

    __declspec(dllexport) void Renderer_RemoveModel(int meshId) {
        g_renderer.RemoveMeshById(meshId);
    }

    __declspec(dllexport) bool Renderer_LoadModel(const char* path, void (*legacyCallback)()) {
        std::string filePath(path);
        std::thread([filePath, legacyCallback]() {
            auto mesh = MeshLoader::Load(filePath);
            if (mesh) {
                int meshId = g_renderer.AddMesh(nullptr);
                g_renderer.UploadMeshToGpu(mesh, meshId);
            }
            if (legacyCallback) legacyCallback();
            }).detach();
        return true;
    }

    // ---------------------------------------------------------------------------
    // 相機
    // ---------------------------------------------------------------------------
    __declspec(dllexport) void Renderer_SetCameraTransform(float px, float py, float pz, float pitch, float yaw) {
        g_renderer.SetCameraTransform(px, py, pz, pitch, yaw);
    }

    // ---------------------------------------------------------------------------
    // 統計
    // ---------------------------------------------------------------------------
    __declspec(dllexport) void Renderer_GetStats(int* vertices, int* polygons, int* drawCalls, float* frameTimeMs) {
        if (vertices && polygons && drawCalls && frameTimeMs)
            g_renderer.GetStats(*vertices, *polygons, *drawCalls, *frameTimeMs);
    }

    // ---------------------------------------------------------------------------
    // Node API  (globalIndex = meshId * MESH_NODE_STRIDE + localIndex)
    // ---------------------------------------------------------------------------
    __declspec(dllexport) int Renderer_GetTotalNodeCount() {
        return g_renderer.GetTotalNodeCount();
    }

    __declspec(dllexport) int Renderer_GetNodeCount() {
        auto mesh = g_renderer.GetMesh();
        return mesh ? (int)mesh->nodes.size() : 0;
    }

    __declspec(dllexport) void Renderer_GetNodeInfo(
        int globalIndex, char* outName, int maxLen, int* outParentGlobalIndex)
    {
        std::string name;
        int parentGlobal = -1;
        if (g_renderer.GetNodeInfo(globalIndex, name, parentGlobal)) {
            if (outName && maxLen > 0) strncpy_s(outName, maxLen, name.c_str(), _TRUNCATE);
            if (outParentGlobalIndex) *outParentGlobalIndex = parentGlobal;
    } else {
        // 查無定節點時，諾輸出空字串供 C# CountNodesForMesh 判斷邊界
            if (outName && maxLen > 0) outName[0] = '\0';
            if (outParentGlobalIndex) *outParentGlobalIndex = -1;
        }
    }

    __declspec(dllexport) void Renderer_GetNodeTransform(
        int globalIndex, float* outT, float* outR, float* outS)
    {
        std::lock_guard<std::mutex> lock(g_renderer.m_renderMutex);
        g_renderer.GetNodeTransform(globalIndex, outT, outR, outS);
    }

    __declspec(dllexport) void Renderer_SetNodeTransform(
        int globalIndex, float* inT, float* inR, float* inS)
    {
        std::lock_guard<std::mutex> lock(g_renderer.m_renderMutex);
        g_renderer.SetNodeTransform(globalIndex, inT, inR, inS);
    }

    __declspec(dllexport) void Renderer_SetAllNodeTransforms(const float* data, int nodeCount) {
        std::lock_guard<std::mutex> lock(g_renderer.m_renderMutex);
    // 每個 entry stride = 11：[globalIndex(float), tx,ty,tz, rx,ry,rz,rw, sx,sy,sz]
        for (int i = 0; i < nodeCount; i++) {
            const float* p = data + i * 11;
            int gIdx = (int)p[0];
            g_renderer.SetNodeTransform(gIdx, p + 1, p + 4, p + 8);
        }
    }

	// Ray Tracing 開關
    __declspec(dllexport) void Renderer_SetRayTracingEnabled(bool enable) {
        g_renderer.SetRayTracingEnabled(enable);
    }

    // ---------------------------------------------------------------------------
    // 光源管理 API (Light System)
    // ---------------------------------------------------------------------------

    __declspec(dllexport) int Renderer_AddLight(int type) {
        std::lock_guard<std::mutex> lock(g_renderer.m_renderMutex);
        return g_renderer.AddLight(type);
    }

    __declspec(dllexport) void Renderer_RemoveLight(int id) {
        std::lock_guard<std::mutex> lock(g_renderer.m_renderMutex);
        g_renderer.RemoveLight(id);
    }

    __declspec(dllexport) bool Renderer_GetLight(
        int id, int* outType, float* outIntensity, float* outConeAngle,
        float* outColor, float* outPos, float* outDir)
    {
        std::lock_guard<std::mutex> lock(g_renderer.m_renderMutex);
        auto* light = g_renderer.GetLight(id);
        if (!light) return false;

        if (outType) *outType = light->type;
        if (outIntensity) *outIntensity = light->intensity;
        if (outConeAngle) *outConeAngle = light->coneAngle;

        if (outColor) {
            outColor[0] = light->color[0];
            outColor[1] = light->color[1];
            outColor[2] = light->color[2];
        }
        if (outPos) {
            outPos[0] = light->position[0];
            outPos[1] = light->position[1];
            outPos[2] = light->position[2];
        }
        if (outDir) {
            outDir[0] = light->direction[0];
            outDir[1] = light->direction[1];
            outDir[2] = light->direction[2];
        }
        return true;
    }

    __declspec(dllexport) bool Renderer_SetLight(
        int id, int type, float intensity, float coneAngle,
        const float* inColor, const float* inPos, const float* inDir)
    {
        std::lock_guard<std::mutex> lock(g_renderer.m_renderMutex);
        auto* light = g_renderer.GetLight(id);
        if (!light) return false;

        light->type = type;
        light->intensity = intensity;
        light->coneAngle = coneAngle;

        if (inColor) {
            light->color[0] = inColor[0];
            light->color[1] = inColor[1];
            light->color[2] = inColor[2];
        }
        if (inPos) {
            light->position[0] = inPos[0];
            light->position[1] = inPos[1];
            light->position[2] = inPos[2];
        }
        if (inDir) {
            light->direction[0] = inDir[0];
            light->direction[1] = inDir[1];
            light->direction[2] = inDir[2];
        }
        return true;
    }

	// ---------------------------------------------------------------------------
	// 環境貼圖
	// ---------------------------------------------------------------------------
    __declspec(dllexport) void Renderer_LoadEnvironmentMap(const wchar_t* path) {
        std::wstring filePath(path);
        g_renderer.LoadEnvironmentMap(filePath);
    }

} // extern "C"