using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Linq;
using UI.Services;

namespace UI.ViewModels;

/// <summary>
/// 場景內一個 Node 的資料節點。
/// GlobalIndex = MeshId * MeshNodeStride + LocalIndex
/// </summary>
internal sealed class NodeItem
{
    public string Name        { get; init; } = string.Empty;
    public int    MeshId      { get; init; }  // 所屬模型
    public int    GlobalIndex { get; init; }  // = MeshId * STRIDE + localIndex
    public int    CppIndex    { get; init; }  // 舊相容，等於 GlobalIndex
    public int    ParentIndex { get; init; }  // parent GlobalIndex，-1 為根
    public ObservableCollection<NodeItem> Children { get; } = new();
}

/// <summary>
/// 管理 Hierarchy 面板的節點樹狀結構與選取狀態。
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

    // ── 建立 / 進行 ──────────────────────────────────────

    /// <summary>建立建立場景内所有 mesh 的完整 Hierarchy。</summary>
    public void Rebuild(RendererService renderer)
    {
        RootNodes.Clear();
        // 目前將在 AddMeshNodes 逐次追加，此處留空備用
    }

    /// <summary>
    /// 將一個模型的所有 node 追加到 Hierarchy。
    /// localNodeCount 為該 mesh 的 node 總數。
    /// </summary>
    public void AddMeshNodes(RendererService renderer, int meshId, int localNodeCount)
    {
        // globalIndex 對應表：用於從樹狀結構中快速查找
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
                CppIndex    = globalIndex, // 舊相容
                ParentIndex = parentGlobal,
            };
            indexMap[globalIndex] = item;

            if (parentGlobal == -1)
                RootNodes.Add(item);
            else if (indexMap.TryGetValue(parentGlobal, out var parent))
                parent.Children.Add(item);
        }
    }

    /// <summary>移除指定 meshId 的所有 NodeItem。</summary>
    public void RemoveMeshNodes(int meshId)
    {
        var toRemove = RootNodes.Where(n => n.MeshId == meshId).ToList();
        foreach (var n in toRemove) RootNodes.Remove(n);
        // 局部子節點隨父節點一同被移除。
        // 若子節點從屬於不同 mesh，則不會發生（模型封閉內部）。
    }
}
