#pragma once
#include "pch.h"

class HDRILoader {
public:
    static Microsoft::WRL::ComPtr<ID3D12Resource> LoadHDR(
        ID3D12Device* device,
        ID3D12GraphicsCommandList* cmdList,
        const std::wstring& filename,
        Microsoft::WRL::ComPtr<ID3D12Resource>& uploadBuffer);
};
