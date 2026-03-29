using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using System;
using System.Collections.Generic;
using System.Numerics;
using System.Threading.Tasks;
using UI.Input;
using UI.ViewModels;

namespace UI;

/// <summary>
/// MainWindow：純 UI 層，只負責事件轉發與 ViewModel 綁定。
/// 不包含任何業務邏輯、相機數學或 P/Invoke 呼叫。
/// </summary>
public sealed partial class MainWindow : Window
{
    private readonly MainViewModel _vm;
    private CameraInputHandler?    _cameraInput;

    // TreeViewNode → NodeItem 映射表
    private readonly Dictionary<TreeViewNode, NodeItem> _nodeMap = new();

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
        // 取得當前螢幕的縮放比例
        double scale = RenderPanel.XamlRoot.RasterizationScale;

        // Init 需要接收實體像素 (Physical Pixels)
        int w = (int)(RenderPanel.ActualWidth * scale);
        int h = (int)(RenderPanel.ActualHeight * scale);

        if (w == 0 || h == 0) return;

        bool ok = _vm.Renderer.Init(RenderPanel, w, h);
        StatusText.Text = ok ? "DX12 Ready ✓" : "DX12 Init Failed ✗";

        if (ok)
        {
            // Init 成功後，立刻呼叫一次 Resize 以套用反向縮放矩陣
            _vm.Renderer.Resize(RenderPanel.ActualWidth, RenderPanel.ActualHeight, scale);

            _cameraInput = new CameraInputHandler(
                RenderPanel,
                _vm.Camera,
                GetSelectedNodeWorldPosition);
        }
    }

    /// <summary>
    /// 提供給 CameraInputHandler 的回呼：
    /// 回傳目前選取節點的世界 Translation，沒有選取則回傳 null。
    /// </summary>
    private Vector3? GetSelectedNodeWorldPosition()
    {
        var node = _vm.Hierarchy.SelectedNode;
        if (node == null) return null;

        var (t, _, _) = _vm.Renderer.GetNodeTransform(node.CppIndex);
        return new Vector3(t[0], t[1], t[2]);
    }

    private void RenderPanel_SizeChanged(object sender, SizeChangedEventArgs e)
    {
        double scale = RenderPanel.XamlRoot.RasterizationScale;
        _vm.Renderer.Resize(e.NewSize.Width, e.NewSize.Height, scale);
    }

    private void RenderPanel_Unloaded(object sender, RoutedEventArgs e)
        => _vm.Renderer.Shutdown();

    // ── 模型載入 ─────────────────────────────────────────

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
        await _vm.Renderer.LoadModelAsync(file.Path);
        _vm.IsLoading = false;
        RebuildHierarchyTree();
    }

    // ── Hierarchy ────────────────────────────────────────

    private void RebuildHierarchyTree()
    {
        _nodeMap.Clear();
        HierarchyTree.RootNodes.Clear();
        _vm.Hierarchy.Rebuild(_vm.Renderer);
        AppendTreeNodes(_vm.Hierarchy.RootNodes, HierarchyTree.RootNodes);
    }

    private void AppendTreeNodes(
        System.Collections.ObjectModel.ObservableCollection<NodeItem> source,
        IList<TreeViewNode> target)
    {
        foreach (var item in source)
        {
            var node = new TreeViewNode { Content = item.Name, IsExpanded = true };
            _nodeMap[node] = item;
            target.Add(node);
            if (item.Children.Count > 0)
                AppendTreeNodes(item.Children, node.Children);
        }
    }

    private void HierarchyTree_ItemInvoked(TreeView sender, TreeViewItemInvokedEventArgs args)
    {
        if (args.InvokedItem is TreeViewNode treeNode &&
            _nodeMap.TryGetValue(treeNode, out var nodeItem))
        {
            _vm.Hierarchy.SelectedNode = nodeItem;
        }
    }

    private void OnNodeSelected(NodeItem? node)
    {
        if (node == null) return;
        NodeNameText.Text   = _vm.Transform.NodeName;
        PosXText.Text       = _vm.Transform.PX.ToString("F3");
        PosYText.Text       = _vm.Transform.PY.ToString("F3");
        PosZText.Text       = _vm.Transform.PZ.ToString("F3");
        RotXText.Text       = _vm.Transform.RX.ToString("F3");
        RotYText.Text       = _vm.Transform.RY.ToString("F3");
        RotZText.Text       = _vm.Transform.RZ.ToString("F3");
        ScaleXText.Text     = _vm.Transform.SX.ToString("F3");
        ScaleYText.Text     = _vm.Transform.SY.ToString("F3");
        ScaleZText.Text     = _vm.Transform.SZ.ToString("F3");
    }

    // ── Transform Inspector ───────────────────────────────

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
}
