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

} // extern "C"
