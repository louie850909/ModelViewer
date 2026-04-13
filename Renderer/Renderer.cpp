#include "pch.h"
#include "Renderer.h"
#include "GeometryPass.h"
#include "DeferredLightPass.h"
#include "ForwardTransparentPass.h"
#include "RayTracingPass.h"
#include "Helper.h"
#include <stdexcept>
#include <filesystem>
#include <string>
#include <algorithm>
#include <stb_image.h>
#include "HDRILoader.h"

#define CHECK(hr) if(FAILED(hr)) throw std::runtime_error("DX12 HRESULT failed")

std::wstring GetShaderPath(const std::wstring& filename) {
    wchar_t buffer[MAX_PATH];
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    std::filesystem::path exePath(buffer);
    return (exePath.parent_path() / L"shaders" / filename).wstring();
}

bool Renderer::Init(IUnknown* panelUnknown, int width, int height) {
    try {
        if (!m_ctx.Init(panelUnknown, width, height)) return false;

        auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        auto cbDesc = CD3DX12_RESOURCE_DESC::Buffer((sizeof(LightBufferData) + 255) & ~255);
        m_ctx.GetDevice()->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &cbDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_lightCB));
        m_lightCB->Map(0, nullptr, (void**)&m_mappedLightCB);

        auto cbDescPass = CD3DX12_RESOURCE_DESC::Buffer((sizeof(PassConstants) + 255) & ~255);
        m_ctx.GetDevice()->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &cbDescPass, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_passCameraCB));
        m_passCameraCB->Map(0, nullptr, (void**)&m_mappedPassCameraCB);

        m_gBuffer.Init(m_ctx.GetDevice(), width, height);

        // 實例化與初始化各渲染階段 (Passes)
        m_geomPass = std::make_unique<GeometryPass>();
        m_lightPass = std::make_unique<DeferredLightPass>();
        m_transparentPass = std::make_unique<ForwardTransparentPass>();
        if (m_ctx.IsDxrSupported())
        {
            m_rayTracingPass = std::make_unique<RayTracingPass>();
            m_temporalDenoiserPass = std::make_unique<TemporalDenoiserPass>();
            m_spatialDenoiserPass = std::make_unique<SpatialDenoiserPass>();
        }

        m_geomPass->Init(m_ctx.GetDevice());
        m_lightPass->Init(m_ctx.GetDevice());
        m_transparentPass->Init(m_ctx.GetDevice());
		if (m_rayTracingPass) m_rayTracingPass->Init(m_ctx.GetDevice());
        if (m_temporalDenoiserPass) m_temporalDenoiserPass->Init(m_ctx.GetDevice());
        if (m_spatialDenoiserPass)
        {
            m_spatialDenoiserPass->SetTemporalPass(m_temporalDenoiserPass.get()); // 綁定關聯
            m_spatialDenoiserPass->Init(m_ctx.GetDevice());
        }
        m_postProcessPass = std::make_unique<PostProcessPass>();
        m_postProcessPass->Init(m_ctx.GetDevice());

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
    if (m_isShuttingDown) {
        m_renderMutex.unlock();
        return;
    }
    m_ctx.Resize(width, height, scale);
    if (m_ctx.GetDevice() != nullptr && width > 0 && height > 0) {
        m_gBuffer.Resize(m_ctx.GetDevice(), m_ctx.GetWidth(), m_ctx.GetHeight());
    }
}

void Renderer::GetStats(int& vertices, int& polygons, int& drawCalls, float& frameTimeMs) {
    vertices = m_statVertices.load(std::memory_order_relaxed);
    polygons = m_statPolygons.load(std::memory_order_relaxed);
    drawCalls = m_statDrawCalls.load(std::memory_order_relaxed);
    frameTimeMs = m_statFrameTime.load(std::memory_order_relaxed);
}

void Renderer::UpdateLightBuffer() {
    m_mappedLightCB->numLights = (int)m_scene.GetLights().size();
    m_mappedLightCB->cameraPos = m_scene.GetCameraPos();
    for (size_t i = 0; i < m_scene.GetLights().size() && i < 16; ++i) {
        const auto& l = m_scene.GetLights()[i];
        m_mappedLightCB->lights[i].type = l.type;
        m_mappedLightCB->lights[i].intensity = l.intensity;
        m_mappedLightCB->lights[i].coneAngle = l.coneAngle;
        m_mappedLightCB->lights[i].color[0] = l.color[0];
        m_mappedLightCB->lights[i].color[1] = l.color[1];
        m_mappedLightCB->lights[i].color[2] = l.color[2];
        m_mappedLightCB->lights[i].position[0] = l.position[0];
        m_mappedLightCB->lights[i].position[1] = l.position[1];
        m_mappedLightCB->lights[i].position[2] = l.position[2];
        m_mappedLightCB->lights[i].direction[0] = l.direction[0];
        m_mappedLightCB->lights[i].direction[1] = l.direction[1];
        m_mappedLightCB->lights[i].direction[2] = l.direction[2];
    }
}

// ---------------------------------------------------------------------------
// RenderFrame (核心渲染迴圈)
// ---------------------------------------------------------------------------
void Renderer::RenderFrame() {
    if (m_isShuttingDown) return;

    auto now = std::chrono::high_resolution_clock::now();
    std::chrono::duration<float, std::milli> dt = now - m_lastFrameTime;
    m_lastFrameTime = now;
    m_statFrameTime.store(dt.count(), std::memory_order_relaxed);

    m_renderMutex.lock();

    UpdateLightBuffer();

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

    using namespace DirectX;
    XMMATRIX rotation = XMMatrixRotationRollPitchYaw(m_scene.GetPitch(), m_scene.GetYaw(), 0.0f);
    XMVECTOR eye = XMLoadFloat3(&m_scene.GetCameraPos());

    // 初始化繪製資料載體
    RenderPassContext passCtx;
    passCtx.gfx = &m_ctx;
    passCtx.scene = &m_scene;
    passCtx.gbuffer = &m_gBuffer;
    passCtx.lightCB = m_lightCB.Get();
    passCtx.forward = XMVector3TransformNormal(XMVectorSet(0, 0, 1, 0), rotation);
    passCtx.view = XMMatrixLookAtLH(eye, eye + passCtx.forward, XMVectorSet(0, 1, 0, 0));

    // 1. 計算無 Jitter 投影矩陣
    passCtx.unjitteredProj = XMMatrixPerspectiveFovLH(XMConvertToRadians(45.f), vp.Width / vp.Height, 0.1f, 5000.f);

    // 判斷是否需要啟用 Jitter (只有在光追/Temporal Pass 開啟時才需要)
    bool useTemporalJitter = (m_rayTracingEnabled && m_ctx.IsDxrSupported());

    if (useTemporalJitter) {
        // 2. 產生 Jitter 偏移
        int jitterPhaseCount = 16;
        int jitterIndex = (m_frameCount % jitterPhaseCount) + 1;
        passCtx.jitterX = Helper::CreateHaltonSequence(jitterIndex, 2) - 0.5f;
        passCtx.jitterY = Helper::CreateHaltonSequence(jitterIndex, 3) - 0.5f;

        float ndcJitterX = (passCtx.jitterX * 2.0f) / vp.Width;
        float ndcJitterY = (passCtx.jitterY * 2.0f) / vp.Height;

        // 3. 套用 Jitter 到最終投影矩陣
        XMFLOAT4X4 projFloat4x4;
        XMStoreFloat4x4(&projFloat4x4, passCtx.unjitteredProj);
        projFloat4x4._31 += ndcJitterX;
        projFloat4x4._32 += ndcJitterY;
        passCtx.proj = XMLoadFloat4x4(&projFloat4x4);
    }
    else {
        // 傳統管線：不套用 Jitter，直接使用原始投影矩陣
        passCtx.jitterX = 0.0f;
        passCtx.jitterY = 0.0f;
        passCtx.proj = passCtx.unjitteredProj;
    }

    passCtx.frameCount = m_frameCount++;

    // 4. 處理上一幀歷史紀錄
    if (passCtx.frameCount == 0) { // 第一幀
        m_prevView = passCtx.view;
        m_prevProj = passCtx.proj;
        m_prevUnjitteredProj = passCtx.unjitteredProj;
    }
    passCtx.prevView = m_prevView;
    passCtx.prevProj = m_prevProj;
    passCtx.prevUnjitteredProj = m_prevUnjitteredProj;

    // 5. 計算並寫入 PassConstants
    XMMATRIX viewProj = passCtx.view * passCtx.proj;
    XMMATRIX unjitteredViewProj = passCtx.view * passCtx.unjitteredProj;
    XMMATRIX prevUnjitteredVP = passCtx.prevView * passCtx.prevUnjitteredProj;

    XMStoreFloat4x4(&m_mappedPassCameraCB->viewProj, XMMatrixTranspose(viewProj));
    XMStoreFloat4x4(&m_mappedPassCameraCB->unjitteredViewProj, XMMatrixTranspose(unjitteredViewProj));
    XMStoreFloat4x4(&m_mappedPassCameraCB->prevUnjitteredViewProj, XMMatrixTranspose(prevUnjitteredVP));
    m_mappedPassCameraCB->envIntegral = m_hdriResource ? m_hdriResource->envIntegral : 1.0f;
    passCtx.passCameraCBAddress = m_passCameraCB->GetGPUVirtualAddress();

    m_geomPass->Execute(cmdList, passCtx);
    if (m_rayTracingEnabled && m_ctx.IsDxrSupported()) {
        // 進入光線追蹤管線
        m_rayTracingPass->Execute(cmdList, passCtx);
        m_temporalDenoiserPass->Execute(cmdList, passCtx);
        m_spatialDenoiserPass->Execute(cmdList, passCtx);

        // 取得降噪後的組合結果
        passCtx.rawDiffuseGI = m_spatialDenoiserPass->GetDenoisedOutput();
        m_postProcessPass->SetSharpness(0.6f);
        m_postProcessPass->Execute(cmdList, passCtx);
    }
    else {
        // 進入傳統光柵化管線
        m_lightPass->Execute(cmdList, passCtx);
        m_transparentPass->Execute(cmdList, passCtx);
    }

    m_statVertices.store(passCtx.totalVerts, std::memory_order_relaxed);
    m_statPolygons.store(passCtx.totalPolys, std::memory_order_relaxed);
    m_statDrawCalls.store(passCtx.currentDrawCalls, std::memory_order_relaxed);

    m_ctx.ExecuteCommandListAndPresent();

    m_prevView = passCtx.view;
    m_prevProj = passCtx.proj;
    m_prevUnjitteredProj = passCtx.unjitteredProj;
    m_renderMutex.unlock();
}

void Renderer::LoadEnvironmentMap(const std::wstring& path) {
    std::lock_guard<std::mutex> lock(m_renderMutex);
    m_ctx.WaitForGpu();
    m_ctx.ResetCommandList();
    auto cmdList = m_ctx.GetCommandList();
    auto cmdQueue = m_ctx.GetCommandQueue();

    // 接收新的資源結構
    m_hdriResource = HDRILoader::LoadHDR(m_ctx.GetDevice(), cmdList, path);
    if (!m_hdriResource) return;

    cmdList->Close();
    ID3D12CommandList* lists[] = { cmdList };
    cmdQueue->ExecuteCommandLists(1, lists);
    m_ctx.WaitForGpu();

    if (m_rayTracingPass) {
        m_rayTracingPass->SetEnvironmentMap(m_hdriResource);
    }
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
    std::vector<TextureCpuData> baseColors(numMaterials), metallicRoughness(numMaterials), normalMaps(numMaterials);
    for (size_t i = 0; i < numMaterials; i++) {
        baseColors[i] = PrepareTextureData(mesh->texturePaths[i], 0xFFFFFFFF);
        metallicRoughness[i] = PrepareTextureData(
            i < mesh->metallicRoughnessPaths.size() ? mesh->metallicRoughnessPaths[i] : "",
            0xFF00FF00);
        // 讀取法線貼圖，若無則給予平坦法線預設值 (RGB = 128, 128, 255 -> 0xFFFF8080)
        normalMaps[i] = PrepareTextureData(
            i < mesh->normalPaths.size() ? mesh->normalPaths[i] : "",
            0xFFFF8080);
    }

    std::lock_guard<std::mutex> lock(m_renderMutex);
    m_ctx.WaitForGpu();

    MeshInstance inst;
    inst.meshId = meshId;
    inst.mesh = mesh;

    auto device = m_ctx.GetDevice();
    UINT srvDescSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    auto cmdList = m_ctx.GetCommandList();
    auto cmdQueue = m_ctx.GetCommandQueue();

    auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    auto defaultHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

    UINT64 vbSize = mesh->vertices.size() * sizeof(Vertex);
    UINT64 ibSize = mesh->indices.size() * sizeof(uint32_t);

    auto vbDesc = CD3DX12_RESOURCE_DESC::Buffer(vbSize);
    device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &vbDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&mesh->vertexBuffer));
    device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &vbDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&inst.vbUpload));

    void* mapped;
    inst.vbUpload->Map(0, nullptr, &mapped);
    memcpy(mapped, mesh->vertices.data(), vbSize);
    inst.vbUpload->Unmap(0, nullptr);

    auto ibDesc = CD3DX12_RESOURCE_DESC::Buffer(ibSize);
    device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &ibDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&mesh->indexBuffer));
    device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &ibDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&inst.ibUpload));

    inst.ibUpload->Map(0, nullptr, &mapped);
    memcpy(mapped, mesh->indices.data(), ibSize);
    inst.ibUpload->Unmap(0, nullptr);

    // 重置 CommandList 準備上傳
    m_ctx.ResetCommandList();

    cmdList->CopyResource(mesh->vertexBuffer.Get(), inst.vbUpload.Get());
    cmdList->CopyResource(mesh->indexBuffer.Get(), inst.ibUpload.Get());
    D3D12_RESOURCE_BARRIER barriers[2] = {
        CD3DX12_RESOURCE_BARRIER::Transition(mesh->vertexBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_GENERIC_READ),
        CD3DX12_RESOURCE_BARRIER::Transition(mesh->indexBuffer.Get(),  D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_GENERIC_READ),
    };
    cmdList->ResourceBarrier(2, barriers);

    // ========================================================
    // 如果支援 DXR，則順便建置此 Mesh 的 BLAS
    // ========================================================
    std::vector<ComPtr<ID3D12Resource>> scratchBuffers; // 暫存，等待 GPU 執行完畢後自然釋放

    if (m_ctx.IsDxrSupported()) {
        auto device5 = m_ctx.GetDevice5();
        mesh->blasBuffers.resize(mesh->subMeshes.size());
        scratchBuffers.resize(mesh->subMeshes.size());

        for (size_t i = 0; i < mesh->subMeshes.size(); ++i) {
            const auto& sub = mesh->subMeshes[i];

            D3D12_RAYTRACING_GEOMETRY_DESC geomDesc = {};
            geomDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;

            // ★ 核心修正 1：加上指標偏移！只為這個 SubMesh 的範圍建置加速結構
            geomDesc.Triangles.IndexBuffer = mesh->indexBuffer->GetGPUVirtualAddress() + sub.indexOffset * sizeof(uint32_t);
            geomDesc.Triangles.IndexCount = sub.indexCount;
            geomDesc.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
            geomDesc.Triangles.Transform3x4 = 0;
            geomDesc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
            geomDesc.Triangles.VertexCount = (UINT)mesh->vertices.size();
            geomDesc.Triangles.VertexBuffer.StartAddress = mesh->vertexBuffer->GetGPUVirtualAddress();
            geomDesc.Triangles.VertexBuffer.StrideInBytes = sizeof(Vertex);
            geomDesc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;

            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
            inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
            inputs.NumDescs = 1;
            inputs.pGeometryDescs = &geomDesc;
            inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;

            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
            device5->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);

            auto defaultHeapForBlas = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
            auto scratchDesc = CD3DX12_RESOURCE_DESC::Buffer(info.ScratchDataSizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            auto blasDesc = CD3DX12_RESOURCE_DESC::Buffer(info.ResultDataMaxSizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

            device5->CreateCommittedResource(&defaultHeapForBlas, D3D12_HEAP_FLAG_NONE, &scratchDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&scratchBuffers[i]));
            device5->CreateCommittedResource(&defaultHeapForBlas, D3D12_HEAP_FLAG_NONE, &blasDesc, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nullptr, IID_PPV_ARGS(&mesh->blasBuffers[i]));

            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {};
            buildDesc.Inputs = inputs;
            buildDesc.DestAccelerationStructureData = mesh->blasBuffers[i]->GetGPUVirtualAddress();
            buildDesc.ScratchAccelerationStructureData = scratchBuffers[i]->GetGPUVirtualAddress();

            ComPtr<ID3D12GraphicsCommandList4> cmdList4;
            if (SUCCEEDED(cmdList->QueryInterface(IID_PPV_ARGS(&cmdList4)))) {
                cmdList4->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);
                // 等待這個 SubMesh 的 BLAS 建置完成
                auto uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(mesh->blasBuffers[i].Get());
                cmdList->ResourceBarrier(1, &uavBarrier);
            }
        }
    }

    cmdList->Close();

    ID3D12CommandList* lists[] = { cmdList };
    cmdQueue->ExecuteCommandLists(1, lists);
    m_ctx.WaitForGpu();

    mesh->vbView = { mesh->vertexBuffer->GetGPUVirtualAddress(), (UINT)vbSize, sizeof(Vertex) };
    mesh->ibView = { mesh->indexBuffer->GetGPUVirtualAddress(),  (UINT)ibSize, DXGI_FORMAT_R32_UINT };

    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.NumDescriptors = numMaterials * 3;
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
        srvHandle.Offset(1, srvDescSize);
        };

    for (size_t i = 0; i < numMaterials; i++) {
        UploadToGPU(baseColors[i]);
        UploadToGPU(metallicRoughness[i]);
        UploadToGPU(normalMaps[i]);
    }

    cmdList->Close();
    ID3D12CommandList* cmds[] = { cmdList };
    cmdQueue->ExecuteCommandLists(1, cmds);
    m_ctx.WaitForGpu();

    // 將資料安插進獨立的場景模組
    m_scene.AddMeshInstance(std::move(inst));
}

void Renderer::RemoveMeshById(int meshId) {
    std::lock_guard<std::mutex> lock(m_renderMutex);
    m_ctx.WaitForGpu();
    m_scene.RemoveMeshById(meshId);
}