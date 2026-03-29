using System;
using System.Runtime.InteropServices;
using System.Threading.Tasks;
using WinRT;
using Microsoft.UI.Xaml.Controls;

namespace UI.Services;

/// <summary>
/// 封裝對 Renderer.dll 的所有 P/Invoke 呼叫與生命週期管理。
/// MainWindow 只需持有此 Service，不直接接觸 RenderBridge。
/// </summary>
internal sealed class RendererService : IDisposable
{
    private IntPtr _panelPtr = IntPtr.Zero;
    private bool _initialized = false;
    private RenderBridge.LoadCallback? _loadCallback;

    public bool IsInitialized => _initialized;

    // ── 生命週期 ─────────────────────────────────────────

    public bool Init(SwapChainPanel panel, int width, int height)
    {
        if (_initialized) return true;

        _panelPtr = MarshalInspectable<SwapChainPanel>.FromManaged(panel);
        Marshal.AddRef(_panelPtr);

        _initialized = RenderBridge.Renderer_Init(_panelPtr, width, height);
        if (!_initialized)
        {
            Marshal.Release(_panelPtr);
            _panelPtr = IntPtr.Zero;
        }
        return _initialized;
    }

    public void Resize(double width, double height, double rasterizationScale)
    {
        if (!_initialized) return;

        // 計算出正確的實體像素 (Physical Pixels)
        int w = (int)(width * rasterizationScale);
        int h = (int)(height * rasterizationScale);

        // 將實體尺寸與縮放比例一起傳給 C++
        if (w > 0 && h > 0)
        {
            RenderBridge.Renderer_Resize(w, h, (float)rasterizationScale);
        }
    }

    public void Shutdown()
    {
        if (!_initialized) return;
        RenderBridge.Renderer_Shutdown();
        _initialized = false;

        if (_panelPtr != IntPtr.Zero)
        {
            Marshal.Release(_panelPtr);
            _panelPtr = IntPtr.Zero;
        }
    }

    public void Dispose() => Shutdown();

    // ── 相機 ─────────────────────────────────────────────

    public void SetCamera(float px, float py, float pz, float pitch, float yaw)
    {
        if (_initialized)
            RenderBridge.Renderer_SetCameraTransform(px, py, pz, pitch, yaw);
    }

    // ── 統計 ─────────────────────────────────────────────

    public (int vertices, int polygons, int drawCalls, float frameTimeMs) GetStats()
    {
        if (!_initialized) return (0, 0, 0, 0f);
        RenderBridge.Renderer_GetStats(out int v, out int p, out int dc, out float ft);
        return (v, p, dc, ft);
    }

    // ── 模型載入 ─────────────────────────────────────────

    /// <summary>
    /// 非同步載入模型。載入完成後在呼叫端 await 處繼續執行。
    /// </summary>
    public Task LoadModelAsync(string path)
    {
        var tcs = new TaskCompletionSource();

        // 保留 Delegate 生命週期到 callback 被呼叫為止
        _loadCallback = new RenderBridge.LoadCallback(() => tcs.TrySetResult());
        RenderBridge.Renderer_LoadModel(path, _loadCallback);
        return tcs.Task;
    }

    // ── Hierarchy ────────────────────────────────────────

    public int GetNodeCount() =>
        _initialized ? RenderBridge.Renderer_GetNodeCount() : 0;

    public (string name, int parentIndex) GetNodeInfo(int index)
    {
        byte[] buf = new byte[256];
        RenderBridge.Renderer_GetNodeInfo(index, buf, buf.Length, out int parent);
        int len = Array.IndexOf(buf, (byte)0);
        if (len < 0) len = buf.Length;
        return (System.Text.Encoding.UTF8.GetString(buf, 0, len), parent);
    }

    // ── Transform ────────────────────────────────────────

    public (float[] t, float[] r, float[] s) GetNodeTransform(int index)
    {
        float[] t = new float[3], r = new float[4], s = new float[3];
        RenderBridge.Renderer_GetNodeTransform(index, t, r, s);
        return (t, r, s);
    }

    public void SetNodeTransform(int index, float[] t, float[] r, float[] s)
        => RenderBridge.Renderer_SetNodeTransform(index, t, r, s);
}
