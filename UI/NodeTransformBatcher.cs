using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace UI;

/// <summary>
/// 每個 Node 的 TRS 資料封包。
/// Rotation 使用 quaternion (x, y, z, w)，與 C++ SceneNode 完全對應。
/// </summary>
internal readonly struct NodeEntry
{
    public readonly int   GlobalIndex;      // globalIndex 傳給 C++
    public readonly float TX, TY, TZ;       // Translation
    public readonly float RX, RY, RZ, RW;  // Rotation (quaternion)
    public readonly float SX, SY, SZ;       // Scale

    public NodeEntry(
        int   globalIndex,
        float tx, float ty, float tz,
        float rx, float ry, float rz, float rw,
        float sx, float sy, float sz)
    {
        GlobalIndex = globalIndex;
        TX = tx; TY = ty; TZ = tz;
        RX = rx; RY = ry; RZ = rz; RW = rw;
        SX = sx; SY = sy; SZ = sz;
    }

    /// <summary>
    /// 從 RendererService.GetNodeTransform() 回傳的 (t, r, s) 陣列建立。
    /// </summary>
    public static NodeEntry FromArrays(int globalIndex, float[] t, float[] r, float[] s)
        => new(globalIndex,
               t[0], t[1], t[2],
               r[0], r[1], r[2], r[3],
               s[0], s[1], s[2]);
}

/// <summary>
/// 將所有 Node 的 TRS 資料打包成單一 pinned float[]，
/// 透過一次 P/Invoke (Renderer_SetAllNodeTransforms) 傳遞給 C++。
///
/// 格式：每個 Node 佔 11 個 float
///   [0]    GlobalIndex  (as float cast)
///   [1..3] Translation  (x, y, z)
///   [4..7] Rotation     (x, y, z, w)  — quaternion
///   [8..10] Scale       (x, y, z)
///
/// 生命週期：隨 MainViewModel 建立並 Dispose。
/// </summary>
internal sealed class NodeTransformBatcher : IDisposable
{
    private const int Stride = 11; // floats per node

    private float[]  _buffer   = Array.Empty<float>();
    private GCHandle _handle;
    private IntPtr   _ptr      = IntPtr.Zero;
    private bool     _disposed = false;

    /// <summary>
    /// 將 entries 列表的 TRS 寫入 pinned buffer，並以單次 P/Invoke 刷入 C++。
    /// </summary>
    public void FlushToCpp(IList<NodeEntry> entries)
    {
        if (_disposed) return;

        int nodeCount = entries.Count;
        if (nodeCount == 0) return;

        EnsureBuffer(nodeCount);

        for (int i = 0; i < nodeCount; i++)
        {
            int b = i * Stride;
            NodeEntry n = entries[i];

            _buffer[b + 0]  = (float)n.GlobalIndex; // globalIndex

            _buffer[b + 1]  = n.TX;
            _buffer[b + 2]  = n.TY;
            _buffer[b + 3]  = n.TZ;

            _buffer[b + 4]  = n.RX;
            _buffer[b + 5]  = n.RY;
            _buffer[b + 6]  = n.RZ;
            _buffer[b + 7]  = n.RW;

            _buffer[b + 8]  = n.SX;
            _buffer[b + 9]  = n.SY;
            _buffer[b + 10] = n.SZ;
        }

        RenderBridge.Renderer_SetAllNodeTransforms(_ptr, nodeCount);
    }

    private void EnsureBuffer(int nodeCount)
    {
        int required = nodeCount * Stride;
        if (_buffer.Length >= required) return;

        if (_handle.IsAllocated)
            _handle.Free();

        _buffer = new float[required];
        _handle = GCHandle.Alloc(_buffer, GCHandleType.Pinned);
        _ptr    = _handle.AddrOfPinnedObject();
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        if (_handle.IsAllocated)
            _handle.Free();
        _ptr = IntPtr.Zero;
    }
}
