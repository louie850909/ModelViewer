using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using System;
using System.Numerics;
using System.Runtime.InteropServices;
using Windows.System;
using UI;
using WinRT;
using Microsoft.UI.Xaml.Media;
using System.Collections.Generic;

namespace UI;

public sealed partial class MainWindow : Window
{
    private bool _rendererInitialized = false;
    private IntPtr _swapChainPanelPtr = IntPtr.Zero;

    // --- 相機狀態 (Source of Truth) ---
    private Vector3 _cameraPos = new Vector3(0, 0, -3f);
    private float _yaw = 0.0f;
    private float _pitch = 0.0f;
    private float _orbitRadius = 3.0f;
    private float _fpsMoveSpeed = 0.05f;

    // --- 輸入狀態 ---
    private bool _isOrbiting = false;
    private bool _isFPSLooking = false;
    private Windows.Foundation.Point _lastMousePos;

    // [極度重要] 宣告一個類別層級的變數來抓住 Delegate，避免被 GC 回收！
    private RenderBridge.LoadCallback _loadCallback;

    // 用來記錄 TreeViewNode 對應到的 C++ 陣列索引
    private Dictionary<TreeViewNode, int> _nodeIndexMap = new();

    public MainWindow()
    {
        InitializeComponent();

        // 註冊類似 Unity Update() 的每幀更新事件
        CompositionTarget.Rendering += GameLoop_Update;

        // 初始化這個 Delegate，指派給我們寫好的方法
        _loadCallback = new RenderBridge.LoadCallback(OnModelLoaded);
    }

    // ==========================================
    // C# 端遊戲迴圈 (處理 WASD 平滑移動與同步狀態)
    // ==========================================
    private void GameLoop_Update(object? sender, object e)
    {
        if (!_rendererInitialized) return;

        // 如果正處於右鍵第一人稱視角，處理 WASD 移動
        if (_isFPSLooking)
        {
            float speed = _fpsMoveSpeed;
            float dR = 0, dU = 0, dF = 0;

            if (IsKeyPressed(VirtualKey.W)) dF += speed;
            if (IsKeyPressed(VirtualKey.S)) dF -= speed;
            if (IsKeyPressed(VirtualKey.A)) dR -= speed;
            if (IsKeyPressed(VirtualKey.D)) dR += speed;
            if (IsKeyPressed(VirtualKey.E)) dU += speed;
            if (IsKeyPressed(VirtualKey.Q)) dU -= speed;

            if (dR != 0 || dU != 0 || dF != 0)
            {
                // 利用 System.Numerics 計算當前的 Forward 與 Right 向量
                Matrix4x4 rot = Matrix4x4.CreateFromYawPitchRoll(_yaw, _pitch, 0f);
                Vector3 forward = Vector3.Transform(Vector3.UnitZ, rot);
                Vector3 right = Vector3.Transform(Vector3.UnitX, rot);
                Vector3 up = Vector3.UnitY;

                _cameraPos += right * dR + up * dU + forward * dF;
            }
        }

        // 每幀將最終狀態送到 C++ (C++ 變回一個單純的繪圖工人)
        RenderBridge.Renderer_SetCameraTransform(_cameraPos.X, _cameraPos.Y, _cameraPos.Z, _pitch, _yaw);

        // ---取得並顯示效能數據 ---
        RenderBridge.Renderer_GetStats(out int v, out int p, out int dc, out float ft);

        // 使用 N0 格式化數字，自動加上千位數逗號，方便閱讀大型模型數據
        StatsText.Text = $"Frame Time : {ft:F2} ms\n" +
                         $"Vertices   : {v:N0}\n" +
                         $"Polygons   : {p:N0}\n" +
                         $"Draw Calls : {dc}";
    }

    private bool IsKeyPressed(VirtualKey key)
    {
        return Microsoft.UI.Input.InputKeyboardSource.GetKeyStateForCurrentThread(key)
            .HasFlag(Windows.UI.Core.CoreVirtualKeyStates.Down);
    }

    private void RenderPanel_Loaded(object sender, RoutedEventArgs e)
    {
        int w = (int)RenderPanel.ActualWidth;
        int h = (int)RenderPanel.ActualHeight;
        if (w == 0 || h == 0) return;

        _swapChainPanelPtr = MarshalInspectable<SwapChainPanel>.FromManaged(RenderPanel);
        Marshal.AddRef(_swapChainPanelPtr);
        
        bool ok = RenderBridge.Renderer_Init(_swapChainPanelPtr, w, h);

        StatusText.Text = ok ? "DX12 Ready ✓" : "DX12 Init Failed ✗";
        _rendererInitialized = ok;
        
        if (!ok)
        {
            Marshal.Release(_swapChainPanelPtr);
            _swapChainPanelPtr = IntPtr.Zero;
        }
    }

    private void RenderPanel_SizeChanged(object sender, SizeChangedEventArgs e)
    {
        if (!_rendererInitialized) return;

        // 取得當前螢幕的縮放比例 (例如 1.5 代表 150%)
        double scale = RenderPanel.XamlRoot.RasterizationScale;

        // 轉換為實體像素
        int w = (int)(e.NewSize.Width * scale);
        int h = (int)(e.NewSize.Height * scale);

        if (w > 0 && h > 0) RenderBridge.Renderer_Resize(w, h);
    }

    private void RenderPanel_Unloaded(object sender, RoutedEventArgs e)
    {
        if (_rendererInitialized) RenderBridge.Renderer_Shutdown();
        
        if (_swapChainPanelPtr != IntPtr.Zero)
        {
            Marshal.Release(_swapChainPanelPtr);
            _swapChainPanelPtr = IntPtr.Zero;
        }
    }

    private async void OpenModel_Click(object sender, RoutedEventArgs e)
    {
        if (!_rendererInitialized) return;

        var picker = new Windows.Storage.Pickers.FileOpenPicker();
        var hwnd = WinRT.Interop.WindowNative.GetWindowHandle(this);
        WinRT.Interop.InitializeWithWindow.Initialize(picker, hwnd);

        picker.FileTypeFilter.Add(".fbx");
        picker.FileTypeFilter.Add(".gltf");
        picker.FileTypeFilter.Add(".glb");
        picker.FileTypeFilter.Add(".obj");

        var file = await picker.PickSingleFileAsync();
        if (file != null)
        {
            // 選好檔案了！開啟載入遮罩
            LoadingOverlay.Visibility = Visibility.Visible;

            // 呼叫 C++ 載入，並把 Delegate 傳過去
            RenderBridge.Renderer_LoadModel(file.Path, _loadCallback);
        }
    }

    // 當載入完成時，會觸發這個方法
    private void OnModelLoaded()
    {
        // 因為這個呼叫來自 C++ 的背景 thread，
        // 必須透過 DispatcherQueue 排程回 WinUI 3 的主執行緒才能更新畫面
        this.DispatcherQueue.TryEnqueue(() =>
        {
            // 關閉載入遮罩
            LoadingOverlay.Visibility = Visibility.Collapsed;

            // 更新 Hierarchy 視窗
            UpdateHierarchy();
        });
    }

    private void UpdateHierarchy()
    {
        HierarchyTree.RootNodes.Clear();
        _nodeIndexMap.Clear(); // 清空舊的映射

        int nodeCount = RenderBridge.Renderer_GetNodeCount();
        if (nodeCount == 0) return;

        byte[] nameBuffer = new byte[256];
        var nodeMap = new Dictionary<int, TreeViewNode>();

        for (int i = 0; i < nodeCount; i++)
        {
            RenderBridge.Renderer_GetNodeInfo(i, nameBuffer, nameBuffer.Length, out int parentIndex);
            int len = Array.IndexOf(nameBuffer, (byte)0);
            if (len < 0) len = 256;
            string nodeName = System.Text.Encoding.UTF8.GetString(nameBuffer, 0, len);

            var treeNode = new TreeViewNode() { Content = nodeName, IsExpanded = true };
            nodeMap[i] = treeNode;

            // 記錄這個 UI 節點對應的 C++ index
            _nodeIndexMap[treeNode] = i;

            if (parentIndex == -1)
            {
                HierarchyTree.RootNodes.Add(treeNode);
            }
            else
            {
                if (nodeMap.TryGetValue(parentIndex, out var parentNode))
                {
                    parentNode.Children.Add(treeNode);
                }
            }
        }
    }

    // 當使用者在 Hierarchy 中點擊某個節點時觸發
    private void HierarchyTree_ItemInvoked(TreeView sender, TreeViewItemInvokedEventArgs args)
    {
        if (args.InvokedItem is TreeViewNode selectedNode && _nodeIndexMap.TryGetValue(selectedNode, out int nodeIndex))
        {
            NodeNameText.Text = selectedNode.Content.ToString();

            float[] t = new float[3];
            float[] r = new float[4];
            float[] s = new float[3];

            RenderBridge.Renderer_GetNodeTransform(nodeIndex, t, r, s);

            // 更新 Position UI
            PosXText.Text = $"X: {t[0]:F3}";
            PosYText.Text = $"Y: {t[1]:F3}";
            PosZText.Text = $"Z: {t[2]:F3}";

            // 更新 Scale UI
            ScaleXText.Text = $"X: {s[0]:F3}";
            ScaleYText.Text = $"Y: {s[1]:F3}";
            ScaleZText.Text = $"Z: {s[2]:F3}";

            // 將四元數 (Quaternion) 轉換為人類易讀的尤拉角 (Euler Angles)
            var quaternion = new Quaternion(r[0], r[1], r[2], r[3]);
            var euler = QuaternionToEulerAngles(quaternion);

            // 更新 Rotation UI (顯示度數)
            RotXText.Text = $"X: {euler.X:F2}°";
            RotYText.Text = $"Y: {euler.Y:F2}°";
            RotZText.Text = $"Z: {euler.Z:F2}°";
        }
    }

    // 將 Quaternion 轉為 Euler 角 (Pitch, Yaw, Roll)
    private Vector3 QuaternionToEulerAngles(Quaternion q)
    {
        Vector3 angles = new Vector3();

        // Roll (x-axis rotation)
        double sinr_cosp = 2 * (q.W * q.X + q.Y * q.Z);
        double cosr_cosp = 1 - 2 * (q.X * q.X + q.Y * q.Y);
        angles.X = (float)Math.Atan2(sinr_cosp, cosr_cosp);

        // Pitch (y-axis rotation)
        double sinp = 2 * (q.W * q.Y - q.Z * q.X);
        if (Math.Abs(sinp) >= 1)
            angles.Y = (float)Math.CopySign(Math.PI / 2, sinp); // 使用 90 度
        else
            angles.Y = (float)Math.Asin(sinp);

        // Yaw (z-axis rotation)
        double siny_cosp = 2 * (q.W * q.Z + q.X * q.Y);
        double cosy_cosp = 1 - 2 * (q.Y * q.Y + q.Z * q.Z);
        angles.Z = (float)Math.Atan2(siny_cosp, cosy_cosp);

        // 轉換成度數回傳
        return new Vector3(
            (float)(angles.X * (180.0 / Math.PI)),
            (float)(angles.Y * (180.0 / Math.PI)),
            (float)(angles.Z * (180.0 / Math.PI))
        );
    }

    // ==========================================
    // 滑鼠事件攔截
    // ==========================================
    private void RenderPanel_PointerPressed(object sender, PointerRoutedEventArgs e)
    {
        var pt = e.GetCurrentPoint(RenderPanel);
        var altState = Microsoft.UI.Input.InputKeyboardSource.GetKeyStateForCurrentThread(VirtualKey.Menu);
        bool isAltPressed = altState.HasFlag(Windows.UI.Core.CoreVirtualKeyStates.Down);

        if (pt.Properties.IsLeftButtonPressed && isAltPressed)
        {
            _isOrbiting = true;
            _lastMousePos = pt.Position;
            RenderPanel.CapturePointer(e.Pointer);
        }
        else if (pt.Properties.IsRightButtonPressed)
        {
            _isFPSLooking = true;
            _lastMousePos = pt.Position;
            RenderPanel.CapturePointer(e.Pointer);
        }
    }

    private void RenderPanel_PointerMoved(object sender, PointerRoutedEventArgs e)
    {
        if (!_isOrbiting && !_isFPSLooking) return;

        var pt = e.GetCurrentPoint(RenderPanel);
        float dx = (float)(pt.Position.X - _lastMousePos.X);
        float dy = (float)(pt.Position.Y - _lastMousePos.Y);
        _lastMousePos = pt.Position;
        float sensitivity = 0.005f;

        if (_isOrbiting)
        {
            // 1. 推算目前的中心點 (Pivot)
            Matrix4x4 oldRot = Matrix4x4.CreateFromYawPitchRoll(_yaw, _pitch, 0f);
            Vector3 oldForward = Vector3.Transform(Vector3.UnitZ, oldRot);
            Vector3 pivot = _cameraPos + oldForward * _orbitRadius;

            // 2. 更新角度 (限制 Pitch 避免萬向鎖)
            _yaw += dx * sensitivity;
            _pitch += dy * sensitivity;
            _pitch = Math.Clamp(_pitch, -1.56f, 1.56f);

            // 3. 推算新的攝影機位置
            Matrix4x4 newRot = Matrix4x4.CreateFromYawPitchRoll(_yaw, _pitch, 0f);
            Vector3 newForward = Vector3.Transform(Vector3.UnitZ, newRot);
            _cameraPos = pivot - newForward * _orbitRadius;
        }
        else if (_isFPSLooking)
        {
            _yaw += dx * sensitivity;
            _pitch += dy * sensitivity;
            _pitch = Math.Clamp(_pitch, -1.56f, 1.56f);
        }
    }

    private void RenderPanel_PointerReleased(object sender, PointerRoutedEventArgs e)
    {
        var pt = e.GetCurrentPoint(RenderPanel);

        if (!pt.Properties.IsLeftButtonPressed && _isOrbiting)
        {
            _isOrbiting = false;
            RenderPanel.ReleasePointerCapture(e.Pointer);
        }
        if (!pt.Properties.IsRightButtonPressed && _isFPSLooking)
        {
            _isFPSLooking = false;
            RenderPanel.ReleasePointerCapture(e.Pointer);
        }
    }

    private void RenderPanel_PointerWheelChanged(object sender, PointerRoutedEventArgs e)
    {
        float scrollDelta = e.GetCurrentPoint(RenderPanel).Properties.MouseWheelDelta;

        if (_isFPSLooking)
        {
            // 按住右鍵時，滾輪用來調整「飛行速度」
            // 往上滾 (正值) 速度增加 20%，往下滾 (負值) 速度減少 20%
            float speedMultiplier = scrollDelta > 0 ? 1.2f : 0.8f;

            _fpsMoveSpeed *= speedMultiplier;

            // 限制速度的上下限，避免飛太慢或瞬間衝出宇宙 (可依您的模型單位微調)
            _fpsMoveSpeed = Math.Clamp(_fpsMoveSpeed, 0.001f, 5.0f);
        }
        else
        {
            // 沒有按右鍵時，維持原本的「拉近拉遠 (Zoom)」功能
            float zoomSensitivity = 0.005f;
            float dF = scrollDelta * zoomSensitivity;

            Matrix4x4 rot = Matrix4x4.CreateFromYawPitchRoll(_yaw, _pitch, 0f);
            Vector3 forward = Vector3.Transform(Vector3.UnitZ, rot);

            _cameraPos += forward * dF;
            _orbitRadius = Math.Max(0.1f, _orbitRadius - dF);
        }
    }
}