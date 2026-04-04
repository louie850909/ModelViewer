#pragma once
#include "pch.h"
#include <stdexcept>

// GBuffer layout (2 RTs, depth is read back as SRV for world-pos reconstruction):
//   RT0 (m_albedo)  : RGB = Albedo,  A = Roughness  [R8G8B8A8_UNORM]
//   RT1 (m_normal)  : RGB = Normal,  A = Metallic   [R16G16B16A16_FLOAT]
//   Depth (shared with GraphicsContext DSV, also bound as SRV t2 in Lighting Pass)
//
// WorldPos is reconstructed in DeferredLight.hlsl from depth + inv-view-proj.
class GBuffer {
public:
    void Init(ID3D12Device* device, int width, int height,
              ID3D12Resource* sharedDepthBuffer);
    void Resize(ID3D12Device* device, int width, int height,
                ID3D12Resource* sharedDepthBuffer);
    void Shutdown();

    ID3D12Resource* GetAlbedo() const { return m_albedo.Get(); }
    ID3D12Resource* GetNormal() const { return m_normal.Get(); }

    ID3D12DescriptorHeap* GetRtvHeap() const { return m_rtvHeap.Get(); }
    // SRV heap: slot 0 = albedo, slot 1 = normal, slot 2 = depth (R32_FLOAT)
    ID3D12DescriptorHeap* GetSrvHeap() const { return m_srvHeap.Get(); }

    D3D12_CPU_DESCRIPTOR_HANDLE GetRtvStart() const {
        return m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    }
    D3D12_GPU_DESCRIPTOR_HANDLE GetSrvStart() const {
        return m_srvHeap->GetGPUDescriptorHandleForHeapStart();
    }

    // RT count is now 2 (no WorldPos RT)
    static constexpr int TargetCount = 2;

private:
    void CreateResources(ID3D12Device* device, int width, int height);
    void CreateHeapsAndViews(ID3D12Device* device,
                             ID3D12Resource* sharedDepthBuffer);

    ComPtr<ID3D12Resource> m_albedo;  // RT0: RGB=Albedo, A=Roughness
    ComPtr<ID3D12Resource> m_normal;  // RT1: RGB=Normal, A=Metallic
    // m_worldPos removed; WorldPos reconstructed from depth in shader

    ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    ComPtr<ID3D12DescriptorHeap> m_srvHeap; // 3 slots: albedo, normal, depth

    UINT m_rtvDescSize = 0;
    UINT m_srvDescSize = 0;
};
