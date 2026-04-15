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

    // カメラ
    [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
    public static extern void Renderer_SetCameraTransform(float px, float py, float pz, float pitch, float yaw);

    // 統計情報
    [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
    public static extern void Renderer_GetStats(out int vertices, out int polygons, out int drawCalls, out float frameTimeMs);

    // ── モデルの読み込み / 削除 ─────────────────────────────────────

    /// callback シグネチャ：(int meshId)
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void AddModelCallback(int meshId);

    /// モデルをシーンに追加し、meshId を即座に返す。
    [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
    public static extern int Renderer_AddModel([MarshalAs(UnmanagedType.LPStr)] string path, AddModelCallback callback);

    /// 指定した meshId をシーンから削除する。
    [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
    public static extern void Renderer_RemoveModel(int meshId);

    /// 旧互換 API（モデルの読み込み、旧呼び出し用）
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void LoadCallback();

    [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
    public static extern bool Renderer_LoadModel([MarshalAs(UnmanagedType.LPStr)] string path, LoadCallback callback);

    /// 環境マップを読み込み、シェーダーに提供する。
    [DllImport(DLL, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
    public static extern void Renderer_LoadEnvironmentMap(string path);

    // ── Node API (globalIndex = meshId * MESH_NODE_STRIDE + localIndex) ──

    /// シーン内のすべての mesh のノード総数。
    [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
    public static extern int Renderer_GetTotalNodeCount();

    /// 旧互換：最初の mesh のノード数を取得する。
    [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
    public static extern int Renderer_GetNodeCount();

    [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
    public static extern void Renderer_GetNodeInfo(int globalIndex, byte[] outName, int maxLen, out int outParentGlobalIndex);

    [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
    public static extern void Renderer_GetNodeTransform(int globalIndex, float[] outT, float[] outR, float[] outS);

    [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
    public static extern void Renderer_SetNodeTransform(int globalIndex, float[] t, float[] r, float[] s);

    // Light API
    [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
    public static extern int Renderer_AddLight(int type);
    [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
    public static extern void Renderer_RemoveLight(int id);
    [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
    public static extern bool Renderer_GetLight(int id, out int type, out float intensity, out float coneAngle, [Out] float[] color, [Out] float[] pos, [Out] float[] dir);
    [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
    public static extern bool Renderer_SetLight(int id, int type, float intensity, float coneAngle, float[] color, float[] pos, float[] dir);

    // ── レンダリング設定 ──────────────────────────────────────
    [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
    public static extern void Renderer_SetRayTracingEnabled(bool enable);

    /// <summary>
    /// すべての Node Transform をバッチ更新する。
    /// 各 entry のフォーマット：[globalIndex(float), tx, ty, tz, rx, ry, rz, rw, sx, sy, sz] = 11 floats
    /// </summary>
    [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
    public static extern void Renderer_SetAllNodeTransforms(IntPtr data, int nodeCount);
}
