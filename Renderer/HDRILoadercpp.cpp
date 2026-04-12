#include "pch.h"
#include "HDRILoader.h"
#include "stb_image.h"

ComPtr<ID3D12Resource> HDRILoader::LoadHDR(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList,
    const std::wstring& filename,
    ComPtr<ID3D12Resource>& uploadBuffer)
{
    int width, height, channels;
    FILE* file = nullptr;
    _wfopen_s(&file, filename.c_str(), L"rb");
    if (!file) return nullptr;
    float* data = stbi_loadf_from_file(file, &width, &height, &channels, 4); // 強制轉為 4 通道 (RGBA)

    if (!data) return nullptr;

    UINT rowPitch = width * 4 * sizeof(float);
    UINT imageSize = rowPitch * height;

    // 1. 建立 Default Heap 資源 (GPU 專用記憶體)
    auto defaultHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    auto texDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32G32B32A32_FLOAT, width, height, 1, 1);

    ComPtr<ID3D12Resource> texResource;
    device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texResource));

    // 2. 建立 Upload Heap 資源 (中轉記憶體)
    auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    auto uploadDesc = CD3DX12_RESOURCE_DESC::Buffer(imageSize);
    device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadBuffer));

    // 3. 複製資料到 Upload Buffer
    void* mappedData;
    uploadBuffer->Map(0, nullptr, &mappedData);
    memcpy(mappedData, data, imageSize);
    uploadBuffer->Unmap(0, nullptr);

    // 4. 使用 Command List 執行 GPU 內部複製
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = texResource.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = uploadBuffer.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    src.PlacedFootprint.Footprint.Width = width;
    src.PlacedFootprint.Footprint.Height = height;
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = rowPitch;

    cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    // 5. 轉換 Resource State 到 Shader 可讀
    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(texResource.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cmdList->ResourceBarrier(1, &barrier);

    stbi_image_free(data);
    return texResource;
}