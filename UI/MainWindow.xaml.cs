using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Numerics;
using UI.Input;
using UI.ViewModels;

namespace UI;

public sealed partial class MainWindow : Window
{
    private readonly MainViewModel _vm;
    private CameraInputHandler?    _cameraInput;

    private readonly Dictionary<TreeViewNode, NodeItem>   _nodeMap       = new();
    private readonly Dictionary<int, List<TreeViewNode>>  _meshRootNodes = new();

    public MainWindow()
    {
        InitializeComponent();
        _vm = new MainViewModel();
        _vm.Hierarchy.OnNodeSelected += OnNodeSelected;
        _vm.IsLoadingChanged += isLoading =>
            LoadingOverlay.Visibility = isLoading ? Visibility.Visible : Visibility.Collapsed;
        CompositionTarget.Rendering += OnGameLoopTick;
    }

    // ── GameLoop ─────────────────────────────────────────
    private void OnGameLoopTick(object? sender, object e)
    {
        if (!_vm.Renderer.IsInitialized) return;
        _cameraInput?.TickMovement();
        _vm.Tick();
        StatsText.Text = _vm.Stats.DisplayText;
    }

    // ── Renderer 生命週期 ─────────────────────────────────
    private void RenderPanel_Loaded(object sender, RoutedEventArgs e)
    {
        double scale = RenderPanel.XamlRoot.RasterizationScale;
        int w = (int)(RenderPanel.ActualWidth  * scale);
        int h = (int)(RenderPanel.ActualHeight * scale);
        if (w == 0 || h == 0) return;
        bool ok = _vm.Renderer.Init(RenderPanel, w, h);
        StatusText.Text = ok ? "DX12 Ready ✓" : "DX12 Init Failed ✗";
        if (ok) {
            _vm.Renderer.Resize(RenderPanel.ActualWidth, RenderPanel.ActualHeight, scale);
            _cameraInput = new CameraInputHandler(RenderPanel, _vm.Camera, GetSelectedNodeWorldPosition);
        }
    }

    private Vector3? GetSelectedNodeWorldPosition()
    {
        var node = _vm.Hierarchy.SelectedNode;
        if (node == null) return null;
        var (t, _, _) = _vm.Renderer.GetNodeTransform(node.GlobalIndex);
        return new Vector3(t[0], t[1], t[2]);
    }

    private void RenderPanel_SizeChanged(object sender, SizeChangedEventArgs e)
    {
        double scale = RenderPanel.XamlRoot.RasterizationScale;
        _vm.Renderer.Resize(e.NewSize.Width, e.NewSize.Height, scale);
    }

    private void RenderPanel_Unloaded(object sender, RoutedEventArgs e)
        => _vm.Renderer.Shutdown();

    // ── 模型載入 ───────────────────────────────────────
    private async void OpenModel_Click(object sender, RoutedEventArgs e)
    {
        if (!_vm.Renderer.IsInitialized) return;
        var picker = new Windows.Storage.Pickers.FileOpenPicker();
        var hwnd = WinRT.Interop.WindowNative.GetWindowHandle(this);
        WinRT.Interop.InitializeWithWindow.Initialize(picker, hwnd);
        picker.FileTypeFilter.Add(".fbx");
        picker.FileTypeFilter.Add(".gltf");
        picker.FileTypeFilter.Add(".glb");
        picker.FileTypeFilter.Add(".obj");
        var file = await picker.PickSingleFileAsync().AsTask();
        if (file == null) return;

        _vm.IsLoading = true;
        int meshId = await _vm.Renderer.AddModelAsync(file.Path);
        _vm.IsLoading = false;

        int localCount = CountNodesForMesh(meshId);
        AppendMeshToHierarchy(meshId, localCount);
    }

    private void AppendMeshToHierarchy(int meshId, int localNodeCount)
    {
        _vm.Hierarchy.AddMeshNodes(_vm.Renderer, meshId, localNodeCount);
        var meshRoots = new List<TreeViewNode>();
        _meshRootNodes[meshId] = meshRoots;
        var rootsForThisMesh = _vm.Hierarchy.RootNodes.Where(n => n.MeshId == meshId).ToList();
        AppendTreeNodes(rootsForThisMesh, HierarchyTree.RootNodes, meshRoots);
    }

    private int CountNodesForMesh(int meshId)
    {
        int count  = 0;
        int stride = UI.Services.RendererService.MeshNodeStride;
        byte[] buf = new byte[256];
        for (int local = 0; local < stride; local++) {
            buf[0] = 0;
            RenderBridge.Renderer_GetNodeInfo(meshId * stride + local, buf, buf.Length, out int _);
            if (buf[0] == 0) break;
            count++;
        }
        return count;
    }

    private void AppendTreeNodes(
        IList<NodeItem> source,
        IList<TreeViewNode> target,
        List<TreeViewNode>? rootTracker = null)
    {
        foreach (var item in source) {
            var node = new TreeViewNode { Content = item.Name, IsExpanded = true };
            _nodeMap[node] = item;
            target.Add(node);
            rootTracker?.Add(node);
            if (item.Children.Count > 0)
                AppendTreeNodes(item.Children, node.Children);
        }
    }

    // ── Hierarchy 選取 ─────────────────────────────────────
    private void HierarchyTree_ItemInvoked(TreeView sender, TreeViewItemInvokedEventArgs args)
    {
        if (args.InvokedItem is TreeViewNode treeNode &&
            _nodeMap.TryGetValue(treeNode, out var nodeItem))
            _vm.Hierarchy.SelectedNode = nodeItem;
    }

    private void OnNodeSelected(NodeItem? node)
    {
        if (node == null) return;
        NodeNameText.Text = _vm.Transform.NodeName;
        PosXText.Text     = _vm.Transform.PX.ToString("F3");
        PosYText.Text     = _vm.Transform.PY.ToString("F3");
        PosZText.Text     = _vm.Transform.PZ.ToString("F3");
        RotXText.Text     = _vm.Transform.RX.ToString("F3");
        RotYText.Text     = _vm.Transform.RY.ToString("F3");
        RotZText.Text     = _vm.Transform.RZ.ToString("F3");
        ScaleXText.Text   = _vm.Transform.SX.ToString("F3");
        ScaleYText.Text   = _vm.Transform.SY.ToString("F3");
        ScaleZText.Text   = _vm.Transform.SZ.ToString("F3");
    }

    // ── 右鍵選單 ───────────────────────────────────────

    private void HierarchyTree_RightTapped(object sender, RightTappedRoutedEventArgs e)
    {
        // 1. 從 OriginalSource 往上尋找 TreeViewItem
        var hit = e.OriginalSource as DependencyObject;
        TreeViewItem? tvi = null;
        while (hit != null) {
            if (hit is TreeViewItem t) { tvi = t; break; }
            hit = VisualTreeHelper.GetParent(hit);
        }
        if (tvi == null) return;

        // 2. 重點修正：TreeViewItem.DataContext 是 Content（string）
        //    必須用 TreeView.NodeFromContainer() 反查 TreeViewNode
        var treeNode = HierarchyTree.NodeFromContainer(tvi);
        if (treeNode == null) return;
        if (!_nodeMap.TryGetValue(treeNode, out var nodeItem)) return;

        // 3. 選取該節點
        _vm.Hierarchy.SelectedNode = nodeItem;

        // 4. 建立選單
        var flyout = new MenuFlyout();

        if (nodeItem.ParentIndex != -1) {
            flyout.Items.Add(new MenuFlyoutItem {
                Text      = $"模型：{GetModelRootName(nodeItem.MeshId)}",
                IsEnabled = false,
            });
            flyout.Items.Add(new MenuFlyoutSeparator());
        }

        var deleteItem = new MenuFlyoutItem {
            Text = nodeItem.ParentIndex == -1 ? "切除模型" : "切除整個模型",
        };
        deleteItem.Click += (_, _) => DeleteModel(nodeItem.MeshId);
        flyout.Items.Add(deleteItem);

        flyout.ShowAt(tvi, e.GetPosition(tvi));
        e.Handled = true;
    }

    private string GetModelRootName(int meshId)
    {
        if (_meshRootNodes.TryGetValue(meshId, out var roots) && roots.Count > 0)
            return roots[0].Content?.ToString() ?? $"Mesh {meshId}";
        return $"Mesh {meshId}";
    }

    private void DeleteModel(int meshId)
    {
        _vm.Renderer.RemoveModel(meshId);
        _vm.Hierarchy.RemoveMeshNodes(meshId);
        if (_meshRootNodes.TryGetValue(meshId, out var roots)) {
            foreach (var r in roots) {
                HierarchyTree.RootNodes.Remove(r);
                RemoveFromNodeMap(r);
            }
            _meshRootNodes.Remove(meshId);
        }
        if (_vm.Hierarchy.SelectedNode?.MeshId == meshId)
            _vm.Hierarchy.SelectedNode = null;
    }

    private void RemoveFromNodeMap(TreeViewNode node)
    {
        _nodeMap.Remove(node);
        foreach (var child in node.Children)
            RemoveFromNodeMap(child);
    }

    // ── Transform Inspector ─────────────────────────────────
    private void TransformInput_KeyDown(object sender, KeyRoutedEventArgs e)
    {
        if (e.Key == Windows.System.VirtualKey.Enter) ApplyTransform();
    }

    private void TransformInput_LostFocus(object sender, RoutedEventArgs e)
        => ApplyTransform();

    private void ApplyTransform()
    {
        _vm.Transform.TryApplyFromStrings(
            PosXText.Text,   PosYText.Text,   PosZText.Text,
            RotXText.Text,   RotYText.Text,   RotZText.Text,
            ScaleXText.Text, ScaleYText.Text, ScaleZText.Text);
    }

    // ── 工具 ─────────────────────────────────────────────
    private IEnumerable<NodeItem> GetNodesForMesh(int meshId)
        => _nodeMap.Values.Where(n => n.MeshId == meshId);
}
