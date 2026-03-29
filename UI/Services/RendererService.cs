using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Threading.Tasks;
using WinRT;
using Microsoft.UI.Xaml.Controls;

namespace UI.Services;

/// <summary>
/// 封裝對 Renderer.dll 的所有 P/Invoke 呼叫與生命週期管理。
/// </summary>
internal sealed class RendererService : IDisposable
{
    public const int MeshNodeStride = 10000;

    private IntPtr _panelPtr   = IntPtr.Zero;
    private bool   _initialized = false;
    private RenderBridge.AddModelCallback? _addModelCallback;

    public bool IsInitialized => _initialized;

    // ── 生命週期 ───────────────────────────────────

    public bool Init(SwapChainPanel panel, int width, int height)
    {
        if (_initialized) return true;
        _panelPtr = MarshalInspectable<SwapChainPanel>.FromManaged(panel);
        Marshal.AddRef(_panelPtr);
        _initialized = RenderBridge.Renderer_Init(_panelPtr, width, height);
        if (!_initialized) { Marshal.Release(_panelPtr); _panelPtr = IntPtr.Zero; }
        return _initialized;
    }

    public void Resize(double width, double height, double rasterizationScale)
    {
        if (!_initialized) return;
        int w = (int)(width  * rasterizationScale);
        int h = (int)(height * rasterizationScale);
        if (w > 0 && h > 0) RenderBridge.Renderer_Resize(w, h, (float)rasterizationScale);
    }

    public void Shutdown()
    {
        if (!_initialized) return;
        RenderBridge.Renderer_Shutdown();
        _initialized = false;
        if (_panelPtr != IntPtr.Zero) { Marshal.Release(_panelPtr); _panelPtr = IntPtr.Zero; }
    }

    public void Dispose() => Shutdown();

    // ── 相機 ─────────────────────────────────────────
    public void SetCamera(float px, float py, float pz, float pitch, float yaw)
    {
        if (_initialized) RenderBridge.Renderer_SetCameraTransform(px, py, pz, pitch, yaw);
    }

    // ── 統計 ─────────────────────────────────────────
    public (int vertices, int polygons, int drawCalls, float frameTimeMs) GetStats()
    {
        if (!_initialized) return (0, 0, 0, 0f);
        RenderBridge.Renderer_GetStats(out int v, out int p, out int dc, out float ft);
        return (v, p, dc, ft);
    }

    // ── 模型載入 / 移除 ────────────────────────────────

    public Task<int> AddModelAsync(string path)
    {
        var tcs = new TaskCompletionSource<int>();
        _addModelCallback = new RenderBridge.AddModelCallback(meshId => tcs.TrySetResult(meshId));
        RenderBridge.Renderer_AddModel(path, _addModelCallback);
        return tcs.Task;
    }

    public void RemoveModel(int meshId)
    {
        if (_initialized) RenderBridge.Renderer_RemoveModel(meshId);
    }

    // ── Node (globalIndex = meshId * MeshNodeStride + localIndex) ─────────

    public int GetTotalNodeCount() =>
        _initialized ? RenderBridge.Renderer_GetTotalNodeCount() : 0;

    public IEnumerable<int> GetGlobalIndicesForMesh(int meshId, int nodeCount)
    {
        for (int i = 0; i < nodeCount; i++)
            yield return meshId * MeshNodeStride + i;
    }

    public (string name, int parentGlobalIndex) GetNodeInfo(int globalIndex)
    {
        byte[] buf = new byte[256];
        RenderBridge.Renderer_GetNodeInfo(globalIndex, buf, buf.Length, out int parent);
        int len = Array.IndexOf(buf, (byte)0);
        if (len < 0) len = buf.Length;
        return (System.Text.Encoding.UTF8.GetString(buf, 0, len), parent);
    }

    public (float[] t, float[] r, float[] s) GetNodeTransform(int globalIndex)
    {
        float[] t = new float[3], r = new float[4], s = new float[3];
        RenderBridge.Renderer_GetNodeTransform(globalIndex, t, r, s);
        return (t, r, s);
    }

    public void SetNodeTransform(int globalIndex, float[] t, float[] r, float[] s)
        => RenderBridge.Renderer_SetNodeTransform(globalIndex, t, r, s);

    public void FlushNodeTransforms(NodeTransformBatcher batcher, List<NodeEntry> entries)
    {
        if (!_initialized || entries.Count == 0) return;
        batcher.FlushToCpp(entries);
    }

    // 舊相容
    public int GetNodeCount() =>
        _initialized ? RenderBridge.Renderer_GetNodeCount() : 0;
}
