using System;
using System.Runtime.InteropServices;

namespace UI;

internal static class RenderBridge
{
    private const string DLL = "Renderer.dll";

    [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
    public static extern bool Renderer_Init(IntPtr panelUnknown, int width, int height);

    [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
    public static extern void Renderer_Resize(int width, int height);

    [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
    public static extern bool Renderer_LoadModel([MarshalAs(UnmanagedType.LPStr)] string path);

    [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
    public static extern void Renderer_Shutdown();

    [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
    public static extern void Renderer_SetCameraTransform(float px, float py, float pz, float pitch, float yaw);
}