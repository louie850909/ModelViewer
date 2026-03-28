#include "pch.h"
#include "Renderer.h"
#include "MeshLoader.h"
#include <thread>
#include <atomic>

static Renderer          g_renderer;
static std::thread       g_renderThread;
static std::atomic<bool> g_running{ false };
static std::atomic<bool> g_resizePending{ false };
static std::atomic<int>  g_newW{ 0 }; // 改為 atomic
static std::atomic<int>  g_newH{ 0 }; // 改為 atomic

extern "C" {

    typedef void (*LoadCallback)();

    __declspec(dllexport) bool Renderer_Init(IUnknown* panelUnknown, int width, int height) {
        if (!g_renderer.Init(panelUnknown, width, height)) return false;
        g_running = true;
        g_renderThread = std::thread([]() {
            while (g_running) {
                if (g_resizePending.exchange(false))
                    g_renderer.Resize(g_newW, g_newH); // 內部會自己鎖

                g_renderer.RenderFrame(); // 內部會自己鎖
            }
            });
        return true;
    }

    __declspec(dllexport) void Renderer_Resize(int width, int height) {
        g_newW.store(width);
        g_newH.store(height);
        g_resizePending.store(true);
    }

    __declspec(dllexport) void Renderer_Shutdown() {
        g_running = false;
        if (g_renderThread.joinable()) g_renderThread.join();
        g_renderer.Shutdown();
    }

    __declspec(dllexport) bool Renderer_LoadModel(const char* path, LoadCallback callback) {
        std::string filePath(path);

        // 啟動背景執行緒處理載入
        std::thread([filePath, callback]() {
            auto mesh = MeshLoader::Load(filePath);
            if (mesh) {
                g_renderer.UploadMeshToGpu(mesh);
            }

            // 載入與 GPU 上傳全部完成後，呼叫 C# 傳進來的 Callback
            if (callback) {
                callback();
            }
            }).detach();

        return true;
    }

    __declspec(dllexport) void Renderer_SetCameraTransform(float px, float py, float pz, float pitch, float yaw) {
        g_renderer.SetCameraTransform(px, py, pz, pitch, yaw);
    }

    __declspec(dllexport) void Renderer_GetStats(int* vertices, int* polygons, int* drawCalls, float* frameTimeMs) {
        if (vertices && polygons && drawCalls && frameTimeMs) {
            g_renderer.GetStats(*vertices, *polygons, *drawCalls, *frameTimeMs);
        }
    }

    __declspec(dllexport) int Renderer_GetNodeCount() {
        auto mesh = g_renderer.GetMesh();
        return mesh ? (int)mesh->nodes.size() : 0;
    }

    __declspec(dllexport) void Renderer_GetNodeInfo(int index, char* outName, int maxLen, int* outParentIndex) {
        auto mesh = g_renderer.GetMesh();
        if (mesh && index >= 0 && index < mesh->nodes.size()) {
            const auto& node = mesh->nodes[index];
            if (outParentIndex) *outParentIndex = node.parentIndex;
            if (outName && maxLen > 0) {
                // 安全複製字串，避免溢位
                strncpy_s(outName, maxLen, node.name.c_str(), _TRUNCATE);
            }
        }
    }

    __declspec(dllexport) void Renderer_GetNodeTransform(int index, float* outT, float* outR, float* outS) {
        auto mesh = g_renderer.GetMesh();
        if (mesh && index >= 0 && index < mesh->nodes.size()) {
            const auto& node = mesh->nodes[index];
            if (outT) { outT[0] = node.t[0]; outT[1] = node.t[1]; outT[2] = node.t[2]; }
            if (outR) { outR[0] = node.r[0]; outR[1] = node.r[1]; outR[2] = node.r[2]; outR[3] = node.r[3]; }
            if (outS) { outS[0] = node.s[0]; outS[1] = node.s[1]; outS[2] = node.s[2]; }
        }
    }

} // extern "C"