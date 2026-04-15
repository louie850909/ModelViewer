#pragma once
#include "pch.h"

// HDRI に関連するすべてのリソースをカプセル化
struct HDRIResource {
    Microsoft::WRL::ComPtr<ID3D12Resource> texture;
    Microsoft::WRL::ComPtr<ID3D12Resource> marginalCDF;    // 1D (高さ)
    Microsoft::WRL::ComPtr<ID3D12Resource> conditionalCDF; // 2D (幅 x 高さ)

    // GPU コピーが完了する前にリソースが解放されるのを防ぐため
    Microsoft::WRL::ComPtr<ID3D12Resource> texUpload;
    Microsoft::WRL::ComPtr<ID3D12Resource> marginalUpload;
    Microsoft::WRL::ComPtr<ID3D12Resource> conditionalUpload;

    uint32_t width = 0;
    uint32_t height = 0;
    float envIntegral = 0.0f; // 環境光全体の総エネルギー、PDF 計算に使用
};

class HDRILoader {
public:
    static std::shared_ptr<HDRIResource> LoadHDR(
        ID3D12Device* device,
        ID3D12GraphicsCommandList* cmdList,
        const std::wstring& filename);
};
