#include "pch.h"
#include "Renderer.h"
#include <stdexcept>
#include <filesystem>
#include <string>
#include <algorithm>
#include <stb_image.h>

#define CHECK(hr) if(FAILED(hr)) throw std::runtime_error("DX12 HRESULT failed")

std::wstring GetShaderPath(const std::wstring& filename) {
    wchar_t buffer[MAX_PATH];
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    std::filesystem::path exePath(buffer);
    return (exePath.parent_path() / L"shaders" / filename).wstring();
}

// ---------------------------------------------------------------------------
// Init / Shutdown / Resize
// ---------------------------------------------------------------------------
bool Renderer::Init(IUnknown* panelUnknown, int width, int height) {
    try {
        if (!m_ctx.Init(panelUnknown, width, height)) return false;

        CreateRootSignaturesAndPSOs();
        m_srvDescriptorSize = m_ctx.GetDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        m_gBuffer.Init(m_ctx.GetDevice(), width, height);
        m_lastFrameTime = std::chrono::high_resolution_clock::now();
        return true;
    }
    catch (...) { return false; }
}

void Renderer::Shutdown() {
    m_isShuttingDown = true;

    std::lock_guard<std::mutex> lock(m_renderMutex);
    m_ctx.WaitForGpu();
    m_gBuffer.Shutdown();
    m_ctx.Shutdown();
}

void Renderer::Resize(int width, int height, float scale) {
    if (m_isShuttingDown) return;

    std::lock_guard<std::mutex> lock(m_renderMutex);
    // 拿到鎖之後再次確認，避免在等待鎖的期間 Shutdown 被觸發
    if (m_isShuttingDown) {
        m_renderMutex.unlock();
        return;
    }

    m_ctx.Resize(width, height, scale);

    if (m_ctx.GetDevice() != nullptr && width > 0 && height > 0) {
        // 注意：Resize 時 m_ctx 內部會更新長寬，所以可以用 m_ctx.GetWidth() 取得真實像素大小
        m_gBuffer.Resize(m_ctx.GetDevice(), m_ctx.GetWidth(), m_ctx.GetHeight());
    }
}

void Renderer::GetStats(int& vertices, int& polygons, int& drawCalls, float& frameTimeMs) {
    vertices = m_statVertices.load(std::memory_order_relaxed);
    polygons = m_statPolygons.load(std::memory_order_relaxed);
    drawCalls = m_statDrawCalls.load(std::memory_order_relaxed);
    frameTimeMs = m_statFrameTime.load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// RenderFrame (核心渲染迴圈)
// ---------------------------------------------------------------------------
void Renderer::RenderFrame() {
    // 如果正在關閉，直接退回，不往下執行
    if (m_isShuttingDown) return;

    auto now = std::chrono::high_resolution_clock::now();
    std::chrono::duration<float, std::milli> dt = now - m_lastFrameTime;
    m_lastFrameTime = now;
    m_statFrameTime.store(dt.count(), std::memory_order_relaxed);

    int currentDrawCalls = 0;
    int totalVerts = 0, totalPolys = 0;

    m_renderMutex.lock();

    // 拿到鎖之後再次確認，避免在等待鎖的期間 Shutdown 被觸發
    if (m_isShuttingDown) {
        m_renderMutex.unlock();
        return;
    }

    m_ctx.ResetCommandList();
    auto cmdList = m_ctx.GetCommandList();
    auto vp = m_ctx.GetViewport();
    auto sc = m_ctx.GetScissorRect();
    cmdList->RSSetViewports(1, &vp);
    cmdList->RSSetScissorRects(1, &sc);

    // ==========================================
    // 通道 1：Geometry Pass
    // ==========================================
    // 綁定 G-Buffer RTVs 與 DSV
    D3D12_CPU_DESCRIPTOR_HANDLE gbufferRTVs = m_gBuffer.GetRtvStart();
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_ctx.GetDSV();
    cmdList->OMSetRenderTargets(3, &gbufferRTVs, TRUE, &dsv);

    // 清除 G-Buffer 與 Depth
    float clearGBuffer[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(gbufferRTVs);
    for (int i = 0; i < 3; ++i) {
        cmdList->ClearRenderTargetView(rtvHandle, clearGBuffer, 0, nullptr);
        rtvHandle.Offset(1, m_ctx.GetDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV));
    }
    cmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    cmdList->SetGraphicsRootSignature(m_geomRootSig.Get());
    cmdList->SetPipelineState(m_geomPSO.Get());
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // 準備相機矩陣
    using namespace DirectX;
    XMMATRIX rotation = XMMatrixRotationRollPitchYaw(m_scene.GetPitch(), m_scene.GetYaw(), 0.0f);
    XMVECTOR eye = XMLoadFloat3(&m_scene.GetCameraPos());
    XMVECTOR forward = XMVector3TransformNormal(XMVectorSet(0, 0, 1, 0), rotation);
    XMVECTOR up = XMVectorSet(0, 1, 0, 0);
    XMMATRIX view = XMMatrixLookAtLH(eye, eye + forward, up);
    XMMATRIX proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(45.f), vp.Width / vp.Height, 0.1f, 5000.f);

    // 繪製 Opaque 物件
    for (auto& inst : m_scene.GetMeshes()) {
        auto& mesh = inst.mesh;
        if (!mesh) continue;

        // 計算 Global Transforms
        std::vector<XMMATRIX> globalTransforms(mesh->nodes.size());
        for (size_t i = 0; i < mesh->nodes.size(); ++i) {
            const auto& node = mesh->nodes[i];
            XMMATRIX local =
                XMMatrixScaling(node.s[0], node.s[1], node.s[2]) *
                XMMatrixRotationQuaternion(XMVectorSet(node.r[0], node.r[1], node.r[2], node.r[3])) *
                XMMatrixTranslation(node.t[0], node.t[1], node.t[2]);
            globalTransforms[i] = (node.parentIndex >= 0)
                ? local * globalTransforms[node.parentIndex]
                : local;
        }

        cmdList->IASetVertexBuffers(0, 1, &mesh->vbView);
        cmdList->IASetIndexBuffer(&mesh->ibView);
        ID3D12DescriptorHeap* heaps[] = { inst.srvHeap.Get() };
        cmdList->SetDescriptorHeaps(1, heaps);

        for (size_t n = 0; n < mesh->nodes.size(); ++n) {
            const auto& node = mesh->nodes[n];
            if (node.subMeshIndices.empty()) continue;

            XMMATRIX modelMat = globalTransforms[n];
            SceneConstants cb = {};
            XMStoreFloat4x4(&cb.mvp, XMMatrixTranspose(modelMat * view * proj));
            XMStoreFloat4x4(&cb.modelMatrix, XMMatrixTranspose(modelMat));
            XMStoreFloat4x4(&cb.normalMatrix, XMMatrixInverse(nullptr, modelMat)); // 未考慮非等比縮放
            XMStoreFloat3(&cb.lightDir, XMVector3Normalize(forward));
            cb.cameraPos = m_scene.GetCameraPos();
            cmdList->SetGraphicsRoot32BitConstants(0, sizeof(SceneConstants) / 4, &cb, 0);

            for (int subIdx : node.subMeshIndices) {
                const auto& sub = mesh->subMeshes[subIdx];
                if (sub.isTransparent) continue; // 延遲渲染第一階段只畫不透明物

                int matIdx = (sub.materialIndex >= 0 && sub.materialIndex < (int)mesh->texturePaths.size()) ? sub.materialIndex : 0;
                CD3DX12_GPU_DESCRIPTOR_HANDLE srvHandle(inst.srvHeap->GetGPUDescriptorHandleForHeapStart(), matIdx * 2, m_srvDescriptorSize);
                cmdList->SetGraphicsRootDescriptorTable(1, srvHandle);
                cmdList->DrawIndexedInstanced(sub.indexCount, 1, sub.indexOffset, 0, 0);
                currentDrawCalls++;
            }
        }
        totalVerts += (int)mesh->vertices.size();
        totalPolys += (int)mesh->indices.size() / 3;
    }

    // ==========================================
    // 轉換 G-Buffer 狀態 (RENDER_TARGET -> SRV)
    // ==========================================
    D3D12_RESOURCE_BARRIER barriersToSRV[3] = {
        CD3DX12_RESOURCE_BARRIER::Transition(m_gBuffer.GetAlbedo(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        CD3DX12_RESOURCE_BARRIER::Transition(m_gBuffer.GetNormal(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        CD3DX12_RESOURCE_BARRIER::Transition(m_gBuffer.GetWorldPos(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
    };
    cmdList->ResourceBarrier(3, barriersToSRV);

    // ==========================================
    // 通道 2：Lighting Pass
    // ==========================================
    // 取得並綁定 BackBuffer
    D3D12_CPU_DESCRIPTOR_HANDLE backBufferRTV = m_ctx.GetCurrentBackBufferRTV();
    cmdList->OMSetRenderTargets(1, &backBufferRTV, FALSE, nullptr);

    cmdList->SetGraphicsRootSignature(m_lightRootSig.Get());
    cmdList->SetPipelineState(m_lightPSO.Get());

    // 綁定 G-Buffer 的 SRV (準備給 Shader 讀取)
    ID3D12DescriptorHeap* gbufferHeaps[] = { m_gBuffer.GetSrvHeap() };
    cmdList->SetDescriptorHeaps(1, gbufferHeaps);

    SceneConstants lightCb = {};
    lightCb.cameraPos = m_scene.GetCameraPos();
    XMStoreFloat3(&lightCb.lightDir, XMVector3Normalize(forward));
    cmdList->SetGraphicsRoot32BitConstants(0, sizeof(SceneConstants) / 4, &lightCb, 0);

    cmdList->SetGraphicsRootDescriptorTable(1, m_gBuffer.GetSrvStart());

    // 繪製全螢幕四邊形 (3個頂點產生一個包含整個螢幕的三角形)
    cmdList->DrawInstanced(3, 1, 0, 0);
    currentDrawCalls++;

    // ==========================================
    // 轉換 G-Buffer 狀態 (SRV -> RENDER_TARGET) 供下一幀使用
    // ==========================================
    D3D12_RESOURCE_BARRIER barriersToRTV[3] = {
        CD3DX12_RESOURCE_BARRIER::Transition(m_gBuffer.GetAlbedo(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
        CD3DX12_RESOURCE_BARRIER::Transition(m_gBuffer.GetNormal(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
        CD3DX12_RESOURCE_BARRIER::Transition(m_gBuffer.GetWorldPos(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET)
    };
    cmdList->ResourceBarrier(3, barriersToRTV);

    // ==========================================
    // 通道 3：Forward Pass (處理半透明物件)
    // ==========================================
    // 綁定 BackBuffer (RTV) + Geometry Pass 建立好的 Depth Buffer (DSV)
    cmdList->OMSetRenderTargets(1, &backBufferRTV, FALSE, &dsv);

    cmdList->SetGraphicsRootSignature(m_forwardRootSig.Get());
    cmdList->SetPipelineState(m_transparentPSO.Get());

    // 繪製 Transparent 物件
    for (auto& inst : m_scene.GetMeshes()) {
        auto& mesh = inst.mesh;
        if (!mesh) continue;

        std::vector<XMMATRIX> globalTransforms(mesh->nodes.size());
        for (size_t i = 0; i < mesh->nodes.size(); ++i) {
            const auto& node = mesh->nodes[i];
            XMMATRIX local = XMMatrixScaling(node.s[0], node.s[1], node.s[2]) * XMMatrixRotationQuaternion(XMVectorSet(node.r[0], node.r[1], node.r[2], node.r[3])) * XMMatrixTranslation(node.t[0], node.t[1], node.t[2]);
            globalTransforms[i] = (node.parentIndex >= 0) ? local * globalTransforms[node.parentIndex] : local;
        }

        cmdList->IASetVertexBuffers(0, 1, &mesh->vbView);
        cmdList->IASetIndexBuffer(&mesh->ibView);
        ID3D12DescriptorHeap* heaps[] = { inst.srvHeap.Get() };
        cmdList->SetDescriptorHeaps(1, heaps);

        for (size_t n = 0; n < mesh->nodes.size(); ++n) {
            const auto& node = mesh->nodes[n];
            if (node.subMeshIndices.empty()) continue;

            XMMATRIX modelMat = globalTransforms[n];
            SceneConstants cb = {};
            XMStoreFloat4x4(&cb.mvp, XMMatrixTranspose(modelMat * view * proj));
            XMStoreFloat4x4(&cb.modelMatrix, XMMatrixTranspose(modelMat));
            XMStoreFloat4x4(&cb.normalMatrix, XMMatrixInverse(nullptr, modelMat));
            XMStoreFloat3(&cb.lightDir, XMVector3Normalize(forward));
            cb.cameraPos = m_scene.GetCameraPos();
            cmdList->SetGraphicsRoot32BitConstants(0, sizeof(SceneConstants) / 4, &cb, 0);

            for (int subIdx : node.subMeshIndices) {
                const auto& sub = mesh->subMeshes[subIdx];

                // --- 這裡反過來，只畫半透明物件 ---
                if (!sub.isTransparent) continue;

                int matIdx = (sub.materialIndex >= 0 && sub.materialIndex < (int)mesh->texturePaths.size()) ? sub.materialIndex : 0;
                CD3DX12_GPU_DESCRIPTOR_HANDLE srvHandle(inst.srvHeap->GetGPUDescriptorHandleForHeapStart(), matIdx * 2, m_srvDescriptorSize);
                cmdList->SetGraphicsRootDescriptorTable(1, srvHandle);
                cmdList->DrawIndexedInstanced(sub.indexCount, 1, sub.indexOffset, 0, 0);
                currentDrawCalls++;
            }
        }
    }

    // 更新統計數據與送出
    m_statVertices.store(totalVerts, std::memory_order_relaxed);
    m_statPolygons.store(totalPolys, std::memory_order_relaxed);
    m_statDrawCalls.store(currentDrawCalls, std::memory_order_relaxed);

    m_ctx.ExecuteCommandListAndPresent();
    m_renderMutex.unlock();
}

// ---------------------------------------------------------------------------
// UploadMeshToGpu
// ---------------------------------------------------------------------------
void Renderer::UploadMeshToGpu(std::shared_ptr<Mesh> mesh, int meshId) {
    struct TextureCpuData {
        int width = 1, height = 1;
        UINT mipLevels = 1;
        std::vector<std::vector<uint8_t>> mipData;
    };

    auto PrepareTextureData = [](const std::string& path, uint32_t defaultColor) -> TextureCpuData {
        TextureCpuData data;
        int texChannels = 4;
        stbi_uc* pixels = path.empty() ? nullptr : stbi_load(path.c_str(), &data.width, &data.height, &texChannels, 4);
        if (!pixels) {
            data.width = 1; data.height = 1;
            pixels = (stbi_uc*)&defaultColor;
        }
        data.mipLevels = 1;
        UINT tempW = data.width, tempH = data.height;
        while (tempW > 1 || tempH > 1) { data.mipLevels++; tempW = (std::max)(1u, tempW / 2); tempH = (std::max)(1u, tempH / 2); }
        data.mipData.resize(data.mipLevels);
        data.mipData[0].assign(pixels, pixels + (data.width * data.height * 4));
        UINT currW = data.width, currH = data.height;
        for (UINT m = 1; m < data.mipLevels; ++m) {
            UINT prevW = currW, prevH = currH;
            currW = (std::max)(1u, currW / 2); currH = (std::max)(1u, currH / 2);
            data.mipData[m].resize(currW * currH * 4);
            const uint8_t* src = data.mipData[m - 1].data();
            uint8_t* dst = data.mipData[m].data();
            for (UINT y = 0; y < currH; ++y)
                for (UINT x = 0; x < currW; ++x) {
                    UINT sx = x * 2, sy = y * 2, sx1 = (std::min)(sx + 1, prevW - 1), sy1 = (std::min)(sy + 1, prevH - 1);
                    for (int c = 0; c < 4; ++c)
                        dst[(y * currW + x) * 4 + c] = (src[(sy * prevW + sx) * 4 + c] + src[(sy * prevW + sx1) * 4 + c] + src[(sy1 * prevW + sx) * 4 + c] + src[(sy1 * prevW + sx1) * 4 + c]) / 4;
                }
        }
        if (pixels != (stbi_uc*)&defaultColor) stbi_image_free(pixels);
        return data;
        };

    UINT numMaterials = (std::max)(1, (int)mesh->texturePaths.size());
    std::vector<TextureCpuData> baseColors(numMaterials), metallicRoughness(numMaterials);
    for (size_t i = 0; i < numMaterials; i++) {
        baseColors[i] = PrepareTextureData(mesh->texturePaths[i], 0xFFFFFFFF);
        metallicRoughness[i] = PrepareTextureData(
            i < mesh->metallicRoughnessPaths.size() ? mesh->metallicRoughnessPaths[i] : "",
            0xFF00FF00);
    }

    std::lock_guard<std::mutex> lock(m_renderMutex);
    m_ctx.WaitForGpu();

    MeshInstance inst;
    inst.meshId = meshId;
    inst.mesh = mesh;

    auto device = m_ctx.GetDevice();
    auto cmdList = m_ctx.GetCommandList();
    auto cmdQueue = m_ctx.GetCommandQueue();

    auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    auto defaultHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

    UINT64 vbSize = mesh->vertices.size() * sizeof(Vertex);
    UINT64 ibSize = mesh->indices.size() * sizeof(uint32_t);

    auto vbDesc = CD3DX12_RESOURCE_DESC::Buffer(vbSize);
    device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &vbDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&mesh->vertexBuffer));
    device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &vbDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&inst.vbUpload));

    void* mapped;
    inst.vbUpload->Map(0, nullptr, &mapped);
    memcpy(mapped, mesh->vertices.data(), vbSize);
    inst.vbUpload->Unmap(0, nullptr);

    auto ibDesc = CD3DX12_RESOURCE_DESC::Buffer(ibSize);
    device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &ibDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&mesh->indexBuffer));
    device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &ibDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&inst.ibUpload));

    inst.ibUpload->Map(0, nullptr, &mapped);
    memcpy(mapped, mesh->indices.data(), ibSize);
    inst.ibUpload->Unmap(0, nullptr);

    // 重置 CommandList 準備上傳
    m_ctx.ResetCommandList();

    cmdList->CopyResource(mesh->vertexBuffer.Get(), inst.vbUpload.Get());
    cmdList->CopyResource(mesh->indexBuffer.Get(), inst.ibUpload.Get());
    D3D12_RESOURCE_BARRIER barriers[2] = {
        CD3DX12_RESOURCE_BARRIER::Transition(mesh->vertexBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER),
        CD3DX12_RESOURCE_BARRIER::Transition(mesh->indexBuffer.Get(),  D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDEX_BUFFER),
    };
    cmdList->ResourceBarrier(2, barriers);
    cmdList->Close();

    ID3D12CommandList* lists[] = { cmdList };
    cmdQueue->ExecuteCommandLists(1, lists);
    m_ctx.WaitForGpu();

    mesh->vbView = { mesh->vertexBuffer->GetGPUVirtualAddress(), (UINT)vbSize, sizeof(Vertex) };
    mesh->ibView = { mesh->indexBuffer->GetGPUVirtualAddress(),  (UINT)ibSize, DXGI_FORMAT_R32_UINT };

    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.NumDescriptors = numMaterials * 2;
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    CHECK(device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&inst.srvHeap)));

    m_ctx.ResetCommandList();
    CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle(inst.srvHeap->GetCPUDescriptorHandleForHeapStart());
    std::vector<ComPtr<ID3D12Resource>> uploadBuffers;

    auto UploadToGPU = [&](const TextureCpuData& cpuData) {
        inst.textures.emplace_back();
        uploadBuffers.emplace_back();
        auto texDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, cpuData.width, cpuData.height, 1, (UINT16)cpuData.mipLevels);

        auto dh = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        device->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &texDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&inst.textures.back()));

        UINT64 uploadSize = 0;
        device->GetCopyableFootprints(&texDesc, 0, cpuData.mipLevels, 0, nullptr, nullptr, nullptr, &uploadSize);

        auto uh = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        auto bufDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadSize);
        device->CreateCommittedResource(&uh, D3D12_HEAP_FLAG_NONE, &bufDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadBuffers.back()));

        std::vector<D3D12_SUBRESOURCE_DATA> subresources(cpuData.mipLevels);
        for (UINT m = 0; m < cpuData.mipLevels; ++m) {
            UINT cw = (std::max)(1u, (UINT)(cpuData.width >> m));
            UINT ch = (std::max)(1u, (UINT)(cpuData.height >> m));
            subresources[m] = { cpuData.mipData[m].data(), (LONG_PTR)(cw * 4), (LONG_PTR)(cw * 4 * ch) };
        }

        UpdateSubresources(cmdList, inst.textures.back().Get(), uploadBuffers.back().Get(), 0, 0, cpuData.mipLevels, subresources.data());
        auto bar = CD3DX12_RESOURCE_BARRIER::Transition(inst.textures.back().Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmdList->ResourceBarrier(1, &bar);

        D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Format = texDesc.Format;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = cpuData.mipLevels;
        device->CreateShaderResourceView(inst.textures.back().Get(), &sv, srvHandle);
        srvHandle.Offset(1, m_srvDescriptorSize);
        };

    for (size_t i = 0; i < numMaterials; i++) {
        UploadToGPU(baseColors[i]);
        UploadToGPU(metallicRoughness[i]);
    }

    cmdList->Close();
    ID3D12CommandList* cmds[] = { cmdList };
    cmdQueue->ExecuteCommandLists(1, cmds);
    m_ctx.WaitForGpu();

    // 將資料安插進獨立的場景模組
    m_scene.AddMeshInstance(std::move(inst));
}

// ---------------------------------------------------------------------------
// CreateRootSignatureAndPSOs
// ---------------------------------------------------------------------------
void Renderer::CreateRootSignaturesAndPSOs() {
    auto device = m_ctx.GetDevice();

    // ==========================================
    // 1. Geometry Pass Root Signature & PSO
    // ==========================================
    CD3DX12_DESCRIPTOR_RANGE1 geomSrvRange;
    geomSrvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0); // 貼圖：BaseColor, MR
    CD3DX12_ROOT_PARAMETER1 geomParams[2];
    geomParams[0].InitAsConstants(sizeof(SceneConstants) / 4, 0);
    geomParams[1].InitAsDescriptorTable(1, &geomSrvRange, D3D12_SHADER_VISIBILITY_PIXEL);

    CD3DX12_STATIC_SAMPLER_DESC sampler(0, D3D12_FILTER_ANISOTROPIC);
    sampler.MaxAnisotropy = 16;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC geomRsDesc;
    geomRsDesc.Init_1_1(2, geomParams, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
    ComPtr<ID3DBlob> sigBlob, errBlob;
    D3DX12SerializeVersionedRootSignature(&geomRsDesc, D3D_ROOT_SIGNATURE_VERSION_1_1, &sigBlob, &errBlob);
    device->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&m_geomRootSig));

    ComPtr<ID3DBlob> geomVS, geomPS;
    D3DReadFileToBlob(GetShaderPath(L"GBuffer_VS.cso").c_str(), &geomVS);
    D3DReadFileToBlob(GetShaderPath(L"GBuffer_PS.cso").c_str(), &geomPS);

    D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC geomPsoDesc = {};
    geomPsoDesc.InputLayout = { layout, _countof(layout) };
    geomPsoDesc.pRootSignature = m_geomRootSig.Get();
    geomPsoDesc.VS = CD3DX12_SHADER_BYTECODE(geomVS.Get());
    geomPsoDesc.PS = CD3DX12_SHADER_BYTECODE(geomPS.Get());
    geomPsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    geomPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    geomPsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT); // 不混色
    geomPsoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT); // 開啟深度
    geomPsoDesc.SampleMask = UINT_MAX;
    geomPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    // 輸出到 G-Buffer 的 3 個 Render Targets
    geomPsoDesc.NumRenderTargets = 3;
    geomPsoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    geomPsoDesc.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    geomPsoDesc.RTVFormats[2] = DXGI_FORMAT_R32G32B32A32_FLOAT;
    geomPsoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    geomPsoDesc.SampleDesc.Count = 1;
    device->CreateGraphicsPipelineState(&geomPsoDesc, IID_PPV_ARGS(&m_geomPSO));

    // ==========================================
    // 2. Lighting Pass Root Signature & PSO
    // ==========================================
    CD3DX12_DESCRIPTOR_RANGE1 lightSrvRange;
    lightSrvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0); // 讀取 3 張 G-Buffer
    CD3DX12_ROOT_PARAMETER1 lightParams[2];
    lightParams[0].InitAsConstants(sizeof(SceneConstants) / 4, 0);
    lightParams[1].InitAsDescriptorTable(1, &lightSrvRange, D3D12_SHADER_VISIBILITY_PIXEL);

    CD3DX12_STATIC_SAMPLER_DESC lightSampler(0, D3D12_FILTER_MIN_MAG_MIP_POINT);
    lightSampler.AddressU = lightSampler.AddressV = lightSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC lightRsDesc;
    lightRsDesc.Init_1_1(2, lightParams, 1, &lightSampler, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
    D3DX12SerializeVersionedRootSignature(&lightRsDesc, D3D_ROOT_SIGNATURE_VERSION_1_1, &sigBlob, &errBlob);
    device->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&m_lightRootSig));

    ComPtr<ID3DBlob> lightVS, lightPS;
    D3DReadFileToBlob(GetShaderPath(L"DeferredLight_VS.cso").c_str(), &lightVS);
    D3DReadFileToBlob(GetShaderPath(L"DeferredLight_PS.cso").c_str(), &lightPS);

    D3D12_GRAPHICS_PIPELINE_STATE_DESC lightPsoDesc = {};
    lightPsoDesc.pRootSignature = m_lightRootSig.Get();
    lightPsoDesc.VS = CD3DX12_SHADER_BYTECODE(lightVS.Get());
    lightPsoDesc.PS = CD3DX12_SHADER_BYTECODE(lightPS.Get());
    lightPsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    lightPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    lightPsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    // 關閉深度測試 (全螢幕四邊形不需要)
    lightPsoDesc.DepthStencilState.DepthEnable = FALSE;
    lightPsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    lightPsoDesc.SampleMask = UINT_MAX;
    lightPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    // 輸出到 BackBuffer
    lightPsoDesc.NumRenderTargets = 1;
    lightPsoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    lightPsoDesc.SampleDesc.Count = 1;
    device->CreateGraphicsPipelineState(&lightPsoDesc, IID_PPV_ARGS(&m_lightPSO));

    // ==========================================
    // 3. Forward Transparent Pass Root Signature & PSO
    // ==========================================
    CD3DX12_DESCRIPTOR_RANGE1 fwdSrvRange;
    fwdSrvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0); // 貼圖：BaseColor, MR
    CD3DX12_ROOT_PARAMETER1 fwdParams[2];
    fwdParams[0].InitAsConstants(sizeof(SceneConstants) / 4, 0);
    fwdParams[1].InitAsDescriptorTable(1, &fwdSrvRange, D3D12_SHADER_VISIBILITY_PIXEL);

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC fwdRsDesc;
    fwdRsDesc.Init_1_1(2, fwdParams, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
    D3DX12SerializeVersionedRootSignature(&fwdRsDesc, D3D_ROOT_SIGNATURE_VERSION_1_1, &sigBlob, &errBlob);
    device->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&m_forwardRootSig));

    ComPtr<ID3DBlob> fwdVS, fwdPS;
    D3DReadFileToBlob(GetShaderPath(L"ForwardTransparent_VS.cso").c_str(), &fwdVS);
    D3DReadFileToBlob(GetShaderPath(L"ForwardTransparent_PS.cso").c_str(), &fwdPS);

    D3D12_GRAPHICS_PIPELINE_STATE_DESC transPsoDesc = geomPsoDesc; // 繼承大部分 Geometry 設定
    transPsoDesc.pRootSignature = m_forwardRootSig.Get();
    transPsoDesc.VS = CD3DX12_SHADER_BYTECODE(fwdVS.Get());
    transPsoDesc.PS = CD3DX12_SHADER_BYTECODE(fwdPS.Get());
    transPsoDesc.NumRenderTargets = 1; // 只輸出到 BackBuffer
    transPsoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    transPsoDesc.RTVFormats[1] = DXGI_FORMAT_UNKNOWN;
    transPsoDesc.RTVFormats[2] = DXGI_FORMAT_UNKNOWN;

    // 開啟 Alpha Blending
    D3D12_RENDER_TARGET_BLEND_DESC blendDesc = {};
    blendDesc.BlendEnable = TRUE;
    blendDesc.LogicOpEnable = FALSE;
    blendDesc.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    blendDesc.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    blendDesc.BlendOp = D3D12_BLEND_OP_ADD;
    blendDesc.SrcBlendAlpha = D3D12_BLEND_ONE;
    blendDesc.DestBlendAlpha = D3D12_BLEND_ZERO;
    blendDesc.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blendDesc.LogicOp = D3D12_LOGIC_OP_NOOP;
    blendDesc.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    transPsoDesc.BlendState.RenderTarget[0] = blendDesc;

    // 深度設定：開啟深度測試，但「關閉深度寫入」(唯讀)，避免半透明物件互相遮擋產生破綻
    transPsoDesc.DepthStencilState.DepthEnable = TRUE;
    transPsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    transPsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;

    device->CreateGraphicsPipelineState(&transPsoDesc, IID_PPV_ARGS(&m_transparentPSO));
}