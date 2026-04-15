using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Numerics;
using UI.Input;
using UI.Services;
using UI.ViewModels;
using Windows.System;

namespace UI;

public sealed partial class MainWindow : Window
{
    private readonly MainViewModel _vm;
    private CameraInputHandler? _cameraInput;

    private readonly Dictionary<TreeViewNode, NodeItem> _nodeMap = new();
    private readonly Dictionary<int, List<TreeViewNode>> _meshRootNodes = new();

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

    // ── Renderer ライフサイクル ───────────────────────────
    private void RenderPanel_Loaded(object sender, RoutedEventArgs e)
    {
        double scale = RenderPanel.XamlRoot.RasterizationScale;
        int w = (int)(RenderPanel.ActualWidth * scale);
        int h = (int)(RenderPanel.ActualHeight * scale);
        if (w == 0 || h == 0) return;
        bool ok = _vm.Renderer.Init(RenderPanel, w, h);
        StatusText.Text = ok ? "DX12 Ready ✓" : "DX12 Init Failed ✗";
        if (ok)
        {
            _vm.Renderer.Resize(RenderPanel.ActualWidth, RenderPanel.ActualHeight, scale);
            _cameraInput = new CameraInputHandler(RenderPanel, _vm.Camera, GetSelectedNodeWorldPosition);

            // 起動時にデフォルトで Directional Light を 1 つ追加
            AddLight(0);

            // 絶対パスを解決 (BaseDirectory の末尾は通常スラッシュ付きで、Path.Combine が自動処理)
            string hdrPath = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "Assets", "env.hdr");

            // ファイルの存在を確認（安全フェイルセーフ）
            if (File.Exists(hdrPath))
            {
                _vm.Renderer.LoadEnvironmentMap(hdrPath);
            }
            else
            {
                System.Diagnostics.Debug.WriteLine($"[警告] 環境マップが見つかりません: {hdrPath}");
            }
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

    // ── モデルの読み込み ─────────────────────────────────
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
        int count = 0;
        int stride = UI.Services.RendererService.MeshNodeStride;
        byte[] buf = new byte[256];
        for (int local = 0; local < stride; local++)
        {
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
        foreach (var item in source)
        {
            var node = new TreeViewNode { Content = item.Name, IsExpanded = true };
            _nodeMap[node] = item;
            target.Add(node);
            rootTracker?.Add(node);
            if (item.Children.Count > 0)
                AppendTreeNodes(item.Children, node.Children);
        }
    }

    // ── Ray Tracing 切り替え ─────────────────────────────────
    private void RayTracingToggle_Toggled(object sender, RoutedEventArgs e)
    {
        if (_vm.Renderer.IsInitialized)
        {
            _vm.Renderer.SetRayTracingEnabled(RayTracingToggle.IsOn);
        }
    }

    // ── Hierarchy 選択 ───────────────────────────────────────
    private void HierarchyTree_ItemInvoked(TreeView sender, TreeViewItemInvokedEventArgs args)
    {
        if (args.InvokedItem is TreeViewNode treeNode &&
            _nodeMap.TryGetValue(treeNode, out var nodeItem))
            _vm.Hierarchy.SelectedNode = nodeItem;
    }

    private void OnNodeSelected(NodeItem? node)
    {
        if (node == null)
        {
            LightSettingsPanel.Visibility = Visibility.Collapsed;
            return;
        }

        NodeNameText.Text = _vm.Transform.NodeName;
        PosXText.Text = _vm.Transform.PX.ToString("F3");
        PosYText.Text = _vm.Transform.PY.ToString("F3");
        PosZText.Text = _vm.Transform.PZ.ToString("F3");
        RotXText.Text = _vm.Transform.RX.ToString("F3");
        RotYText.Text = _vm.Transform.RY.ToString("F3");
        RotZText.Text = _vm.Transform.RZ.ToString("F3");
        ScaleXText.Text = _vm.Transform.SX.ToString("F3");
        ScaleYText.Text = _vm.Transform.SY.ToString("F3");
        ScaleZText.Text = _vm.Transform.SZ.ToString("F3");

        // プロパティパネル同期：光源ノードの場合、光源プロパティパネルを表示して値を入力
        LightSettingsPanel.Visibility = node.IsLight ? Visibility.Visible : Visibility.Collapsed;
        if (node.IsLight)
        {
            LightRText.Text = _vm.Transform.ColorR.ToString("F3");
            LightGText.Text = _vm.Transform.ColorG.ToString("F3");
            LightBText.Text = _vm.Transform.ColorB.ToString("F3");
            LightIntensityText.Text = _vm.Transform.Intensity.ToString("F3");
            LightConeText.Text = _vm.Transform.ConeAngle.ToString("F3");
            ConeAnglePanel.Visibility = node.LightType == 2 ? Visibility.Visible : Visibility.Collapsed;
        }
    }

    // ── 右クリックメニュー ────────────────────────────────

    private void HierarchyTree_RightTapped(object sender, RightTappedRoutedEventArgs e)
    {
        // 1. 從 OriginalSource 往上尋找 TreeViewItem
        var hit = e.OriginalSource as DependencyObject;
        TreeViewItem? tvi = null;
        while (hit != null)
        {
            if (hit is TreeViewItem t) { tvi = t; break; }
            hit = VisualTreeHelper.GetParent(hit);
        }

        // 空白部分をクリックした場合 (tvi が null)、光源追加メニューを表示
        if (tvi == null)
        {
            var addLightFlyout = new MenuFlyout();

            var addDirLight = new MenuFlyoutItem { Text = "Directional Light を追加" };
            addDirLight.Click += (_, _) => AddLight(0);

            var addPointLight = new MenuFlyoutItem { Text = "Point Light を追加" };
            addPointLight.Click += (_, _) => AddLight(1);

            var addSpotLight = new MenuFlyoutItem { Text = "Spot Light を追加" };
            addSpotLight.Click += (_, _) => AddLight(2);

            addLightFlyout.Items.Add(addDirLight);
            addLightFlyout.Items.Add(addPointLight);
            addLightFlyout.Items.Add(addSpotLight);

            addLightFlyout.ShowAt(HierarchyTree, e.GetPosition(HierarchyTree));
            e.Handled = true;
            return;
        }

        // 2. TreeViewItem から TreeViewNode を逆引き
        var treeNode = HierarchyTree.NodeFromContainer(tvi);
        if (treeNode == null) return;
        if (!_nodeMap.TryGetValue(treeNode, out var nodeItem)) return;

        // 3. そのノードを選択
        _vm.Hierarchy.SelectedNode = nodeItem;

        // 4. ノード固有のメニューを作成
        var flyout = new MenuFlyout();

        if (nodeItem.IsLight)
        {
            var deleteLightItem = new MenuFlyoutItem { Text = "光源を削除" };
            deleteLightItem.Click += (_, _) => DeleteLight(nodeItem);
            flyout.Items.Add(deleteLightItem);
        }
        else
        {
            if (nodeItem.ParentIndex != -1)
            {
                flyout.Items.Add(new MenuFlyoutItem
                {
                    Text = $"モデル：{GetModelRootName(nodeItem.MeshId)}",
                    IsEnabled = false,
                });
                flyout.Items.Add(new MenuFlyoutSeparator());
            }

            var deleteItem = new MenuFlyoutItem
            {
                Text = nodeItem.ParentIndex == -1 ? "モデルを削除" : "モデル全体を削除",
            };
            deleteItem.Click += (_, _) => DeleteModel(nodeItem.MeshId);
            flyout.Items.Add(deleteItem);
        }

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
        if (_meshRootNodes.TryGetValue(meshId, out var roots))
        {
            foreach (var r in roots)
            {
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

    // ── 光源固有ロジック ──────────────────────────────────

    private void AddLight(int type)
    {
        int id = _vm.Renderer.AddLight(type);
        string[] names = { "Directional Light", "Point Light", "Spot Light" };
        var item = new NodeItem { Name = names[type], IsLight = true, LightId = id, LightType = type, ParentIndex = -1 };
        _vm.Hierarchy.RootNodes.Add(item);

        var node = new TreeViewNode { Content = item.Name, IsExpanded = true };
        _nodeMap[node] = item;
        HierarchyTree.RootNodes.Add(node);
    }

    private void DeleteLight(NodeItem node)
    {
        _vm.Renderer.RemoveLight(node.LightId);
        _vm.Hierarchy.RootNodes.Remove(node);

        var toRemove = HierarchyTree.RootNodes.FirstOrDefault(n => _nodeMap.ContainsKey(n) && _nodeMap[n] == node);
        if (toRemove != null)
        {
            HierarchyTree.RootNodes.Remove(toRemove);
            _nodeMap.Remove(toRemove);
        }

        if (_vm.Hierarchy.SelectedNode == node)
            _vm.Hierarchy.SelectedNode = null;
    }

    private void LightInput_LostFocus(object sender, RoutedEventArgs e) => ApplyLight();

    private void LightInput_KeyDown(object sender, KeyRoutedEventArgs e)
    {
        if (e.Key == VirtualKey.Enter) ApplyLight();
    }

    private void ApplyLight()
    {
        if (float.TryParse(LightRText.Text, out float r)) _vm.Transform.ColorR = r;
        if (float.TryParse(LightGText.Text, out float g)) _vm.Transform.ColorG = g;
        if (float.TryParse(LightBText.Text, out float b)) _vm.Transform.ColorB = b;
        if (float.TryParse(LightIntensityText.Text, out float i)) _vm.Transform.Intensity = i;
        if (float.TryParse(LightConeText.Text, out float c)) _vm.Transform.ConeAngle = c;
        _vm.Transform.Apply();
    }

    // ── Transform Inspector ─────────────────────────────────
    private void TransformInput_KeyDown(object sender, KeyRoutedEventArgs e)
    {
        if (e.Key == VirtualKey.Enter) ApplyTransform();
    }

    private void TransformInput_LostFocus(object sender, RoutedEventArgs e)
        => ApplyTransform();

    private void ApplyTransform()
    {
        _vm.Transform.TryApplyFromStrings(
            PosXText.Text, PosYText.Text, PosZText.Text,
            RotXText.Text, RotYText.Text, RotZText.Text,
            ScaleXText.Text, ScaleYText.Text, ScaleZText.Text);
    }

    // ── ユーティリティ ──────────────────────────────────────
    private IEnumerable<NodeItem> GetNodesForMesh(int meshId)
        => _nodeMap.Values.Where(n => n.MeshId == meshId);
}