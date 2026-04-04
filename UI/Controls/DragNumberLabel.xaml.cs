using Microsoft.UI.Input;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using System;

namespace UI.Controls;

public sealed partial class DragNumberLabel : UserControl
{
    // ── DependencyProperties ────────────────────────────────────────────────
    public static readonly DependencyProperty TextProperty =
        DependencyProperty.Register(
            nameof(Text), typeof(string), typeof(DragNumberLabel),
            new PropertyMetadata("X:", (d, e) =>
                ((DragNumberLabel)d).LabelText.Text = (string)e.NewValue));

    public string Text
    {
        get => (string)GetValue(TextProperty);
        set => SetValue(TextProperty, value);
    }

    /// <summary>每 1 pixel 拖移對應的數值變化量。</summary>
    public float DragSensitivity { get; set; } = 0.1f;

    /// <summary>拖移時回呼，回傳新數值供外部更新 TextBox。</summary>
    public event Action<float>? ValueChanged;

    /// <summary>拖曳開始前由外部提供目前數值。</summary>
    public Func<float>? GetCurrentValue { get; set; }

    // ── 內部狀態 ─────────────────────────────────────────────────────────────
    private bool _isDragging;
    private float _dragStartX;
    private float _valueAtDragStart;

    public DragNumberLabel()
    {
        InitializeComponent();
        Loaded += OnLoaded;
    }

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        // 顯示左右調整游標，給使用者視覺提示
        LabelText.ProtectedCursor = InputSystemCursor.Create(InputSystemCursorShape.SizeWestEast);
    }

    private void OnPointerPressed(object sender, PointerRoutedEventArgs e)
    {
        if (GetCurrentValue == null) return;

        _valueAtDragStart = GetCurrentValue();
        _dragStartX = (float)e.GetCurrentPoint(null).Position.X;
        _isDragging = true;

        LabelText.CapturePointer(e.Pointer);
        e.Handled = true;
    }

    private void OnPointerMoved(object sender, PointerRoutedEventArgs e)
    {
        if (!_isDragging) return;

        float currentX = (float)e.GetCurrentPoint(null).Position.X;
        float delta = (currentX - _dragStartX) * DragSensitivity;
        ValueChanged?.Invoke(_valueAtDragStart + delta);
        e.Handled = true;
    }

    private void OnPointerReleased(object sender, PointerRoutedEventArgs e)
    {
        StopDrag(e.Pointer);
        e.Handled = true;
    }

    private void OnPointerCaptureLost(object sender, PointerRoutedEventArgs e)
        => StopDrag(null);

    private void StopDrag(Pointer? pointer)
    {
        if (!_isDragging) return;
        _isDragging = false;
        if (pointer != null)
            LabelText.ReleasePointerCapture(pointer);
    }
}
