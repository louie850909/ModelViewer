#include "pch.h"
#include "HDRILoader.h"
#include "stb_image.h"
#include <cmath>

// ヘルパー関数：Structured Buffer を作成
static void CreateBuffer(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList,
    const void* data, UINT dataSize,
    ComPtr<ID3D12Resource>& defaultBuffer, ComPtr<ID3D12Resource>& uploadBuffer)
{
    auto defaultHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    auto bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(dataSize);

    device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&defaultBuffer));

    device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadBuffer));

    void* pData;
    uploadBuffer->Map(0, nullptr, &pData);
    memcpy(pData, data, dataSize);
    uploadBuffer->Unmap(0, nullptr);

    cmdList->CopyResource(defaultBuffer.Get(), uploadBuffer.Get());

    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(defaultBuffer.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cmdList->ResourceBarrier(1, &barrier);
}

std::shared_ptr<HDRIResource> HDRILoader::LoadHDR(
    ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, const std::wstring& filename)
{
    auto resource = std::make_shared<HDRIResource>();
    int width, height, channels;

    FILE* file = nullptr;
    _wfopen_s(&file, filename.c_str(), L"rb");
    if (!file) return nullptr;

    float* data = stbi_loadf_from_file(file, &width, &height, &channels, 4);
    fclose(file);
    if (!data) return nullptr;

    resource->width = width;
    resource->height = height;

    // ==========================================
    // 1. HDRI CDF を計算 (重要度サンプリングマップ)
    // ==========================================
    std::vector<float> conditionalCDF(width * height, 0.0f);
    std::vector<float> marginalCDF(height, 0.0f);
    float totalWeight = 0.0f;

    for (int y = 0; y < height; ++y) {
        float rowWeight = 0.0f;
        // 正距円筒図法の緯度角 (極付近のピクセルが引き伸ばされた面積を補正するため)
        float theta = (y + 0.5f) / height * 3.1415926535f;
        float sinTheta = std::sin(theta);

        for (int x = 0; x < width; ++x) {
            int idx = (y * width + x) * 4;
            // 輝度 (Luma) を抽出
            float luma = 0.2126f * data[idx] + 0.7152f * data[idx + 1] + 0.0722f * data[idx + 2];
            rowWeight += luma * sinTheta;
            conditionalCDF[y * width + x] = rowWeight;
        }

        // 条件付き確率を正規化 (0.0 ~ 1.0)
        if (rowWeight > 0.0f) {
            for (int x = 0; x < width; ++x) conditionalCDF[y * width + x] /= rowWeight;
        }
        totalWeight += rowWeight;
        marginalCDF[y] = totalWeight;
    }

    // 周辺確率を正規化 (0.0 ~ 1.0)
    if (totalWeight > 0.0f) {
        for (int y = 0; y < height; ++y) marginalCDF[y] /= totalWeight;
    }

    // HLSL の PDF 計算に使用する総エネルギーを保存 (立体角積分定数)
    resource->envIntegral = totalWeight * (2.0f * 3.1415926535f * 3.1415926535f) / (width * height);

    // ==========================================
    // 2. DX12 リソースを作成 (Texture + Buffers)
    // ==========================================
    // Marginal Buffer を作成
    CreateBuffer(device, cmdList, marginalCDF.data(), (UINT)(marginalCDF.size() * sizeof(float)),
        resource->marginalCDF, resource->marginalUpload);

    // Conditional Buffer を作成
    CreateBuffer(device, cmdList, conditionalCDF.data(), (UINT)(conditionalCDF.size() * sizeof(float)),
        resource->conditionalCDF, resource->conditionalUpload);

    // 元の HDRI Texture2D を作成 (元のロジックを維持)
    UINT rowPitch = width * 4 * sizeof(float);
    UINT imageSize = rowPitch * height;
    auto defaultHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    auto texDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32G32B32A32_FLOAT, width, height, 1, 1);

    device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&resource->texture));

    auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    auto uploadDesc = CD3DX12_RESOURCE_DESC::Buffer(imageSize);
    device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&resource->texUpload));

    void* mappedData;
    resource->texUpload->Map(0, nullptr, &mappedData);
    memcpy(mappedData, data, imageSize);
    resource->texUpload->Unmap(0, nullptr);

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = resource->texture.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = resource->texUpload.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    src.PlacedFootprint.Footprint.Width = width;
    src.PlacedFootprint.Footprint.Height = height;
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = rowPitch;

    cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(resource->texture.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cmdList->ResourceBarrier(1, &barrier);

    stbi_image_free(data);
    return resource;
}