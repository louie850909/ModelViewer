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

    public MainWindow()
    {
        InitializeComponent();

        // 註冊類似 Unity Update() 的每幀更新事件
        CompositionTarget.Rendering += GameLoop_Update;
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
            RenderBridge.Renderer_LoadModel(file.Path);
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