#pragma once
#include "pch.h"
#include <stdexcept>

class GBuffer {
public:
    void Init(ID3D12Device* device, int width, int height);
    void Resize(ID3D12Device* device, int width, int height);
    void Shutdown();

    // 取得底層資源 (如果需要設定 Barrier)
    ID3D12Resource* GetAlbedo() const { return m_albedo.Get(); }
    ID3D12Resource* GetNormalRoughness() const { return m_normalRouness.Get(); }
    ID3D12Resource* GetWorldPosMetallic() const { return m_worldPosMetallic.Get(); }
    ID3D12Resource* GetVelocity() const { return m_velocity.Get(); }

    // 取得 Descriptor Heaps
    ID3D12DescriptorHeap* GetRtvHeap() const { return m_rtvHeap.Get(); }
    ID3D12DescriptorHeap* GetSrvHeap() const { return m_srvHeap.Get(); }

    // 取得 RTV Handle (Geometry Pass 寫入時用)
    D3D12_CPU_DESCRIPTOR_HANDLE GetRtvStart() const {
        return m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    }

    // 取得 SRV Handle (Lighting Pass 讀取時用)
    D3D12_GPU_DESCRIPTOR_HANDLE GetSrvStart() const {
        return m_srvHeap->GetGPUDescriptorHandleForHeapStart();
    }

    static constexpr int TargetCount = 4;

private:
    void CreateResources(ID3D12Device* device, int width, int height);
    void CreateHeapsAndViews(ID3D12Device* device);

    ComPtr<ID3D12Resource> m_albedo;    // RT0: RGBA = Albedo
    ComPtr<ID3D12Resource> m_normalRouness;    // RT1: RGB = Normal, A = Roughness
    ComPtr<ID3D12Resource> m_worldPosMetallic;  // RT2: RGB = WorldPos, A = Metallic
    ComPtr<ID3D12Resource> m_velocity;  // RT3: RG = Velocity (螢幕空間向量)

    ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    ComPtr<ID3D12DescriptorHeap> m_srvHeap;

    UINT m_rtvDescSize = 0;
    UINT m_srvDescSize = 0;
};
