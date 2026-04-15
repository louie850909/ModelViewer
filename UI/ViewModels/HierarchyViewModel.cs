using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Linq;
using UI.Services;

namespace UI.ViewModels;

/// <summary>
/// シーン内の 1 つのノードのデータ。
/// GlobalIndex = MeshId * MeshNodeStride + LocalIndex
/// </summary>
internal sealed class NodeItem
{
    public string Name        { get; init; } = string.Empty;
    public int    MeshId      { get; init; }  // 所属モデル
    public int    GlobalIndex { get; init; }  // = MeshId * STRIDE + localIndex
    public int    CppIndex    { get; init; }  // 旧互換、GlobalIndex と等しい
    public int    ParentIndex { get; init; }  // 親の GlobalIndex、-1 はルート
    public ObservableCollection<NodeItem> Children { get; } = new();
    public bool IsLight { get; init; } = false;
    public int LightId { get; init; } = -1;
    public int LightType { get; init; } = 0;
}

/// <summary>
/// Hierarchy パネルのノードツリー構造と選択状態を管理する。
/// </summary>
internal sealed class HierarchyViewModel : ObservableObject
{
    public ObservableCollection<NodeItem> RootNodes { get; } = new();

    private NodeItem? _selectedNode;
    public NodeItem? SelectedNode
    {
        get => _selectedNode;
        set
        {
            if (_selectedNode == value) return;
            _selectedNode = value;
            OnPropertyChanged();
            OnNodeSelected?.Invoke(_selectedNode);
        }
    }

    public event Action<NodeItem?>? OnNodeSelected;

    // ── 構築 / 実行 ──────────────────────────────────────

    /// <summary>シーン内のすべての mesh の完全な Hierarchy を構築する。</summary>
    public void Rebuild(RendererService renderer)
    {
        RootNodes.Clear();
        // 現在は AddMeshNodes で逐次追加するため、ここは空のまま予備として残す
    }

    /// <summary>
    /// 1 つのモデルのすべてのノードを Hierarchy に追加する。
    /// localNodeCount はその mesh のノード総数。
    /// </summary>
    public void AddMeshNodes(RendererService renderer, int meshId, int localNodeCount)
    {
        // globalIndex 対応表：ツリー構造から高速検索するために使用
        var indexMap = new Dictionary<int, NodeItem>(); // globalIndex → NodeItem

        for (int local = 0; local < localNodeCount; local++)
        {
            int globalIndex = meshId * RendererService.MeshNodeStride + local;
            var (name, parentGlobal) = renderer.GetNodeInfo(globalIndex);

            var item = new NodeItem
            {
                Name        = name,
                MeshId      = meshId,
                GlobalIndex = globalIndex,
                CppIndex    = globalIndex, // 旧互換
                ParentIndex = parentGlobal,
            };
            indexMap[globalIndex] = item;

            if (parentGlobal == -1)
                RootNodes.Add(item);
            else if (indexMap.TryGetValue(parentGlobal, out var parent))
                parent.Children.Add(item);
        }
    }

    /// <summary>指定した meshId のすべての NodeItem を削除する。</summary>
    public void RemoveMeshNodes(int meshId)
    {
        var toRemove = RootNodes.Where(n => n.MeshId == meshId).ToList();
        foreach (var n in toRemove) RootNodes.Remove(n);
        // 子ノードは親ノードと一緒に削除される。
        // 子ノードが異なる mesh に属する場合は発生しない（モデルは内部で閉じている）。
    }
}
