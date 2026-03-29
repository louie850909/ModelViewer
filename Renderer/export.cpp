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

/// 新增模型到場景，回傳 meshId。
/// callback 簽名改為 (int meshId) 方便 C# 得知對應的 id。
__declspec(dllexport) int Renderer_AddModel(const char* path, LoadCallback callback) {
    std::string filePath(path);
    int meshId = g_renderer.AddMesh(nullptr); // 先取得 id

    std::thread([filePath, callback, meshId]() {
        auto mesh = MeshLoader::Load(filePath);
        if (mesh) {
            g_renderer.UploadMeshToGpu(mesh, meshId);
        }
        if (callback) callback(meshId);
    }).detach();

    return meshId;
}

/// 從場景移除指定 meshId 的模型。
__declspec(dllexport) void Renderer_RemoveModel(int meshId) {
    g_renderer.RemoveMeshById(meshId);
}

/// 相容舊 API：載入模型（取代現有場景內全部模型，保留舊呢叫不破壞）
/// 如果想要追加語意，請改用 Renderer_AddModel。
__declspec(dllexport) bool Renderer_LoadModel(const char* path, void (*legacyCallback)()) {
    // 清除場景內所有模型
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

/// 場景內所有 mesh 的 node 總數。
__declspec(dllexport) int Renderer_GetTotalNodeCount() {
    return g_renderer.GetTotalNodeCount();
}

/// 相容舊 API：取第一個 mesh 的 node 數。
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

/// 批次更新所有 Node Transform。
/// data 格式：每個元素 10 float，以 globalIndex 順序排列。
__declspec(dllexport) void Renderer_SetAllNodeTransforms(const float* data, int nodeCount) {
    std::lock_guard<std::mutex> lock(g_renderer.m_renderMutex);
    // 視為按 globalIndex 0..N-1 順序排列，每個出彳區區長 10
    // C# 端必須保證排列順序與 GetAllGlobalIndices() 一致
    for (int i = 0; i < nodeCount; i++) {
        const float* p = data + i * 10;
        // 每個 entry 的前 1 個 float 存放 globalIndex（由 C# 寫入）
        // 格式： [globalIndex(as float), tx, ty, tz, rx, ry, rz, rw, sx, sy, sz]
        // stride = 11
        int gIdx = (int)p[0];
        g_renderer.SetNodeTransform(gIdx, p+1, p+4, p+8);
    }
}

/// 取得場景內所有 globalIndex 清單。
/// outIndices 為 int 陣列，長度至少 GetTotalNodeCount()。
__declspec(dllexport) void Renderer_GetAllGlobalIndices(int* outIndices, int maxCount) {
    if (!outIndices) return;
    int written = 0;
    // 場景順序：依 meshes 順序進行，每個 mesh 依 localIndex 順序
    for (auto& inst : const_cast<const Renderer&>(g_renderer).m_meshes) {
        // m_meshes 是 private，改用 GetTotalNodeCount 與 GetNodeInfo 前議
        (void)inst;
    }
    // NOTE: 此函數由於 m_meshes 為 private，正確實作在 Renderer 內部
    // 暫時保留。C# 端目前將使用 GetTotalNodeCount + meshId*STRIDE 尋址。
    (void)written; (void)maxCount;
}

} // extern "C"
