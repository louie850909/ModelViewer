using System;
using System.Runtime.InteropServices;

namespace UI;

internal static class RenderBridge
{
    private const string DLL = "Renderer.dll";

    [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
    public static extern bool Renderer_Init(IntPtr panelUnknown, int width, int height);

    [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
    public static extern void Renderer_Resize(int width, int height, float scale);

    [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
    public static extern void Renderer_Shutdown();

    // 相機
    [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
    public static extern void Renderer_SetCameraTransform(float px, float py, float pz, float pitch, float yaw);

    // 統計
    [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
    public static extern void Renderer_GetStats(out int vertices, out int polygons, out int drawCalls, out float frameTimeMs);

    // ── 模型載入 / 移除 ──────────────────────────────────────────────

    /// callback 簽名：(int meshId)
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void AddModelCallback(int meshId);

    /// 新增模型到場景，立即回傳 meshId。
    [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
    public static extern int Renderer_AddModel([MarshalAs(UnmanagedType.LPStr)] string path, AddModelCallback callback);

    /// 從場景移除指定 meshId。
    [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
    public static extern void Renderer_RemoveModel(int meshId);

    /// 舊相容 API（載入模型，舊呢叫用）
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void LoadCallback();

    [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
    public static extern bool Renderer_LoadModel([MarshalAs(UnmanagedType.LPStr)] string path, LoadCallback callback);

    // ── Node API (globalIndex = meshId * MESH_NODE_STRIDE + localIndex) ──

    /// 場景內所有 mesh 的 node 總數。
    [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
    public static extern int Renderer_GetTotalNodeCount();

    /// 舊相容：取第一個 mesh 的 node 數。
    [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
    public static extern int Renderer_GetNodeCount();

    [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
    public static extern void Renderer_GetNodeInfo(int globalIndex, byte[] outName, int maxLen, out int outParentGlobalIndex);

    [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
    public static extern void Renderer_GetNodeTransform(int globalIndex, float[] outT, float[] outR, float[] outS);

    [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
    public static extern void Renderer_SetNodeTransform(int globalIndex, float[] t, float[] r, float[] s);

    /// <summary>
    /// 批次更新所有 Node Transform。
    /// 每個 entry 檔式：[globalIndex(float), tx, ty, tz, rx, ry, rz, rw, sx, sy, sz] = 11 floats
    /// </summary>
    [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
    public static extern void Renderer_SetAllNodeTransforms(IntPtr data, int nodeCount);
}
