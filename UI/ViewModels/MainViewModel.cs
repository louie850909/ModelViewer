using System;
using System.Collections.Generic;
using UI.Services;

namespace UI.ViewModels;

/// <summary>
/// すべての ViewModel を集約し、MainWindow が唯一保持するトップレベルオブジェクト。
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
    /// GameLoop から毎フレーム呼ばれる：
    /// 1. カメラ状態をプッシュ
    /// 2. すべての dirty な Node Transform を収集し、単一の P/Invoke で C++ に反映
    /// 3. パフォーマンス統計を更新
    /// </summary>
    public void Tick()
    {
        // 1. カメラ
        var c = Camera;
        Renderer.SetCamera(c.Position.X, c.Position.Y, c.Position.Z, c.Pitch, c.Yaw);

        // 2. dirty を収集してバッチで反映
        if (Transform.IsDirty)
        {
            _dirtyEntries.Clear();

            // シーン内のすべてのノードを走査（全 mesh のグローバル globalIndex）
            // Hierarchy.RootNodes はすべての mesh のルートノードを含み、ツリー全体を再帰的に収集
            CollectAllNodeEntries(Hierarchy.RootNodes);

            Renderer.FlushNodeTransforms(_batcher, _dirtyEntries);
            Transform.ClearDirty();
        }

        // 3. パフォーマンス統計
        var (v, p, dc, ft) = Renderer.GetStats();
        Stats.Update(v, p, dc, ft);
    }

    /// <summary>
    /// ツリー全体の NodeEntry を再帰的に収集する：
    /// - 選択中のノードは VM の最新値を使用（Transform.BuildEntry）
    /// - その他のノードは C++ から既存の値を直接読み取る（GetNodeTransform）
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
