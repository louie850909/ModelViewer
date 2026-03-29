using System;
using System.Collections.Generic;
using UI.Services;

namespace UI.ViewModels;

/// <summary>
/// 聚合所有 ViewModel，是 MainWindow 唯一持有的頂層物件。
/// </summary>
internal sealed class MainViewModel : IDisposable
{
    public RendererService    Renderer  { get; } = new();
    public CameraViewModel    Camera    { get; } = new();
    public StatsViewModel     Stats     { get; } = new();
    public HierarchyViewModel Hierarchy { get; } = new();
    public TransformViewModel Transform { get; }

    private readonly NodeTransformBatcher _batcher      = new();
    private readonly List<NodeEntry>      _dirtyEntries = new();

    private bool _isLoading;
    public bool IsLoading
    {
        get => _isLoading;
        set { _isLoading = value; IsLoadingChanged?.Invoke(_isLoading); }
    }

    public event Action<bool>? IsLoadingChanged;

    public MainViewModel()
    {
        Transform = new TransformViewModel(Renderer);
        Hierarchy.OnNodeSelected += node => Transform.LoadNode(node);
    }

    /// <summary>
    /// 每幀由 GameLoop 呼叫：
    /// 1. 推送相機狀態
    /// 2. 收集所有 dirty node Transform，單次 P/Invoke 刷入 C++
    /// 3. 更新效能統計
    /// </summary>
    public void Tick()
    {
        // 1. 相機
        var c = Camera;
        Renderer.SetCamera(c.Position.X, c.Position.Y, c.Position.Z, c.Pitch, c.Yaw);

        // 2. 收集 dirty 并批次刷入
        if (Transform.IsDirty)
        {
            _dirtyEntries.Clear();

            // 遍走場景內所有節點（所有 mesh 的全域 globalIndex）
            // Hierarchy.RootNodes 已含所有 mesh 的根節點，遞迴收集全樹
            CollectAllNodeEntries(Hierarchy.RootNodes);

            Renderer.FlushNodeTransforms(_batcher, _dirtyEntries);
            Transform.ClearDirty();
        }

        // 3. 效能統計
        var (v, p, dc, ft) = Renderer.GetStats();
        Stats.Update(v, p, dc, ft);
    }

    /// <summary>
    /// 遞迴收集全樹的 NodeEntry：
    /// - 選取節點用 VM 最新值（Transform.BuildEntry）
    /// - 其餘節點直接從 C++ 讀回現有值（GetNodeTransform）
    /// </summary>
    private void CollectAllNodeEntries(
        System.Collections.ObjectModel.ObservableCollection<NodeItem> nodes)
    {
        foreach (var node in nodes)
        {
            NodeEntry entry;
            if (node.GlobalIndex == Transform.NodeIndex && Transform.IsDirty)
                entry = Transform.BuildEntry();
            else {
                var (t, r, s) = Renderer.GetNodeTransform(node.GlobalIndex);
                entry = NodeEntry.FromArrays(node.GlobalIndex, t, r, s);
            }
            _dirtyEntries.Add(entry);

            if (node.Children.Count > 0)
                CollectAllNodeEntries(node.Children);
        }
    }

    public void Dispose() => _batcher.Dispose();
}
