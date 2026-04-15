using System;
using System.Numerics;

namespace UI.ViewModels;

/// <summary>
/// カメラ状態 (Source of Truth)。
/// Yaw/Pitch/Position/OrbitRadius を保持し、Orbit・FPS Look・Zoom・WASD・Focus メソッドを提供する。
/// WinUI / P/Invoke API には直接触れない。
/// </summary>
internal sealed class CameraViewModel : ObservableObject
{
    // ── State ────────────────────────────────────────────
    public Vector3 Position    { get; private set; } = new(0f, 0f, -3f);
    public float   Yaw         { get; private set; } = 0f;
    public float   Pitch       { get; private set; } = 0f;
    public float   OrbitRadius { get; private set; } = 3f;
    public float   MoveSpeed   { get; private set; } = 0.05f;

    private const float PitchLimit = 1.56f;
    private const float MinSpeed   = 0.001f;
    private const float MaxSpeed   = 5.0f;

    // ── 公開操作メソッド ──────────────────────────────────

    /// <summary>Orbit 回転 (Alt + 左ボタンドラッグ)。</summary>
    public void ApplyOrbit(float dx, float dy, float sensitivity = 0.005f)
    {
        Matrix4x4 oldRot = Matrix4x4.CreateFromYawPitchRoll(Yaw, Pitch, 0f);
        Vector3 oldForward = Vector3.Transform(Vector3.UnitZ, oldRot);
        Vector3 pivot = Position + oldForward * OrbitRadius;

        float newYaw   = Yaw   + dx * sensitivity;
        float newPitch = Math.Clamp(Pitch + dy * sensitivity, -PitchLimit, PitchLimit);

        Matrix4x4 newRot = Matrix4x4.CreateFromYawPitchRoll(newYaw, newPitch, 0f);
        Vector3 newForward = Vector3.Transform(Vector3.UnitZ, newRot);

        Yaw      = newYaw;
        Pitch    = newPitch;
        Position = pivot - newForward * OrbitRadius;
    }

    /// <summary>一人称視点回転 (右ボタンドラッグ)。</summary>
    public void ApplyFPSLook(float dx, float dy, float sensitivity = 0.005f)
    {
        Yaw   += dx * sensitivity;
        Pitch  = Math.Clamp(Pitch + dy * sensitivity, -PitchLimit, PitchLimit);
    }

    /// <summary>WASD 移動。毎フレーム一度呼ばれる。</summary>
    public void ApplyMove(float right, float up, float forward)
    {
        if (right == 0f && up == 0f && forward == 0f) return;
        Matrix4x4 rot = Matrix4x4.CreateFromYawPitchRoll(Yaw, Pitch, 0f);
        Vector3 fwd = Vector3.Transform(Vector3.UnitZ, rot);
        Vector3 rgt = Vector3.Transform(Vector3.UnitX, rot);
        Position += rgt * right + Vector3.UnitY * up + fwd * forward;
    }

    /// <summary>マウスホイールズーム (右ボタンなしモード)。</summary>
    public void ApplyZoom(float delta, float sensitivity = 0.005f)
    {
        float dF = delta * sensitivity;
        Matrix4x4 rot = Matrix4x4.CreateFromYawPitchRoll(Yaw, Pitch, 0f);
        Vector3 forward = Vector3.Transform(Vector3.UnitZ, rot);
        Position     += forward * dF;
        OrbitRadius   = Math.Max(0.1f, OrbitRadius - dF);
    }

    /// <summary>右ボタン押下中にマウスホイールで飛行速度を調整する。</summary>
    public void AdjustMoveSpeed(float delta)
    {
        float mult = delta > 0 ? 1.2f : 0.8f;
        MoveSpeed = Math.Clamp(MoveSpeed * mult, MinSpeed, MaxSpeed);
    }

    /// <summary>
    /// 指定した目標位置にフォーカスする。
    /// カメラは現在の距離 (orbitRadius) の後方から目標点を向く。
    /// 後続の Orbit 使用のために OrbitRadius も更新する。
    /// </summary>
    /// <param name="target">目標ノードのワールド座標 (Translation)</param>
    /// <param name="distance">フォーカス後に保持する距離。デフォルト 3.0</param>
    public void FocusOn(Vector3 target, float distance = 3.0f)
    {
        // 既存の方向角度を保ったまま、位置と距離のみ移動
        Matrix4x4 rot = Matrix4x4.CreateFromYawPitchRoll(Yaw, Pitch, 0f);
        Vector3 forward = Vector3.Transform(Vector3.UnitZ, rot);

        OrbitRadius = distance;
        Position    = target - forward * distance;
    }
}
