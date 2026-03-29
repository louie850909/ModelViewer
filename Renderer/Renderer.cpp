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
// Init / Shutdown
// ---------------------------------------------------------------------------
bool Renderer::Init(IUnknown* panelUnknown, int width, int height) {
    m_width = width; m_height = height;
    try {
        CreateDeviceAndQueue();
        CreateSwapChain(panelUnknown, width, height);
        CreateRTV();
        CreateDSV();
        for (UINT i = 0; i < FRAME_COUNT; i++)
            CHECK(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_cmdAllocators[i])));
        CHECK(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_cmdAllocators[0].Get(), nullptr, IID_PPV_ARGS(&m_cmdList)));
        CHECK(m_cmdList->Close());
        CHECK(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
        m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        m_currentFenceValue = 0;
        m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
        CreateRootSignatureAndPSO();
        m_srvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        m_lastFrameTime = std::chrono::high_resolution_clock::now();
        return true;
    }
    catch (...) { return false; }
}

void Renderer::Shutdown() {
    WaitForGpu();
    CloseHandle(m_fenceEvent);
}

// ---------------------------------------------------------------------------
// Scene 管理
// ---------------------------------------------------------------------------
int Renderer::AddMesh(std::shared_ptr<Mesh> mesh) {
    return m_nextMeshId++;
}

void Renderer::RemoveMeshById(int meshId) {
    std::lock_guard<std::mutex> lock(m_renderMutex);
    WaitForGpu();
    auto it = std::find_if(m_meshes.begin(), m_meshes.end(),
        [meshId](const MeshInstance& inst) { return inst.meshId == meshId; });
    if (it != m_meshes.end())
        m_meshes.erase(it);
}

MeshInstance* Renderer::FindInstance(int globalIndex, int& outLocalIndex) {
    int meshId    = globalIndex / MESH_NODE_STRIDE;
    outLocalIndex = globalIndex % MESH_NODE_STRIDE;
    for (auto& inst : m_meshes) {
        if (inst.meshId == meshId &&
            outLocalIndex >= 0 &&
            outLocalIndex < (int)inst.mesh->nodes.size())
            return &inst;
    }
    return nullptr;
}

int Renderer::GetTotalNodeCount() {
    int total = 0;
    for (auto& inst : m_meshes)
        total += (int)inst.mesh->nodes.size();
    return total;
}

bool Renderer::GetNodeInfo(int globalIndex, std::string& outName, int& outParentGlobal) {
    int localIdx;
    auto* inst = FindInstance(globalIndex, localIdx);
    if (!inst) return false;
    const auto& node = inst->mesh->nodes[localIdx];
    outName = node.name;
    outParentGlobal = (node.parentIndex >= 0)
        ? inst->meshId * MESH_NODE_STRIDE + node.parentIndex
        : -1;
    return true;
}

bool Renderer::GetNodeTransform(int globalIndex, float* outT, float* outR, float* outS) {
    int localIdx;
    auto* inst = FindInstance(globalIndex, localIdx);
    if (!inst) return false;
    const auto& node = inst->mesh->nodes[localIdx];
    if (outT) { outT[0]=node.t[0]; outT[1]=node.t[1]; outT[2]=node.t[2]; }
    if (outR) { outR[0]=node.r[0]; outR[1]=node.r[1]; outR[2]=node.r[2]; outR[3]=node.r[3]; }
    if (outS) { outS[0]=node.s[0]; outS[1]=node.s[1]; outS[2]=node.s[2]; }
    return true;
}

bool Renderer::SetNodeTransform(int globalIndex, const float* inT, const float* inR, const float* inS) {
    int localIdx;
    auto* inst = FindInstance(globalIndex, localIdx);
    if (!inst) return false;
    auto& node = inst->mesh->nodes[localIdx];
    if (inT) { node.t[0]=inT[0]; node.t[1]=inT[1]; node.t[2]=inT[2]; }
    if (inR) { node.r[0]=inR[0]; node.r[1]=inR[1]; node.r[2]=inR[2]; node.r[3]=inR[3]; }
    if (inS) { node.s[0]=inS[0]; node.s[1]=inS[1]; node.s[2]=inS[2]; }
    return true;
}

// ---------------------------------------------------------------------------
// RenderFrame
// ---------------------------------------------------------------------------
void Renderer::RenderFrame() {
    auto now = std::chrono::high_resolution_clock::now();
    std::chrono::duration<float, std::milli> dt = now - m_lastFrameTime;
    m_lastFrameTime = now;
    m_statFrameTime.store(dt.count(), std::memory_order_relaxed);

    int currentDrawCalls = 0;
    int totalVerts = 0, totalPolys = 0;

    m_renderMutex.lock();

    auto& alloc = m_cmdAllocators[m_frameIndex];
    alloc->Reset();
    m_cmdList->Reset(alloc.Get(), nullptr);

    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        m_renderTargets[m_frameIndex].Get(),
        D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    m_cmdList->ResourceBarrier(1, &barrier);

    CD3DX12_CPU_DESCRIPTOR_HANDLE rtv(m_rtvHeap->GetCPUDescriptorHandleForHeapStart(), (INT)m_frameIndex, m_rtvDescSize);
    float clearColor[] = { 0.2f, 0.2f, 0.2f, 1.0f };
    m_cmdList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
    CD3DX12_CPU_DESCRIPTOR_HANDLE dsv(m_dsvHeap->GetCPUDescriptorHandleForHeapStart());
    m_cmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    using namespace DirectX;

    XMMATRIX rotation = XMMatrixRotationRollPitchYaw(m_pitch, m_yaw, 0.0f);
    XMVECTOR eye     = XMLoadFloat3(&m_cameraPos);
    XMVECTOR forward = XMVector3TransformNormal(XMVectorSet(0, 0, 1, 0), rotation);
    XMVECTOR up      = XMVectorSet(0, 1, 0, 0);
    XMMATRIX view    = XMMatrixLookAtLH(eye, eye + forward, up);
    XMMATRIX proj    = XMMatrixPerspectiveFovLH(XMConvertToRadians(45.f), (float)m_width / m_height, 0.1f, 5000.f);

    D3D12_VIEWPORT vp = { 0,0,(float)m_width,(float)m_height,0,1 };
    D3D12_RECT     sc = { 0,0,m_width,m_height };
    m_cmdList->RSSetViewports(1, &vp);
    m_cmdList->RSSetScissorRects(1, &sc);
    m_cmdList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    m_cmdList->SetGraphicsRootSignature(m_rootSig.Get());
    m_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    for (auto& inst : m_meshes) {
        auto& mesh = inst.mesh;
        if (!mesh) continue;

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

        m_cmdList->IASetVertexBuffers(0, 1, &mesh->vbView);
        m_cmdList->IASetIndexBuffer(&mesh->ibView);

        ID3D12DescriptorHeap* heaps[] = { inst.srvHeap.Get() };
        m_cmdList->SetDescriptorHeaps(1, heaps);

        auto drawPass = [&](bool drawTransparent) {
            for (size_t n = 0; n < mesh->nodes.size(); ++n) {
                const auto& node = mesh->nodes[n];
                if (node.subMeshIndices.empty()) continue;

                XMMATRIX modelMat = globalTransforms[n];

                SceneConstants cb = {};
                XMStoreFloat4x4(&cb.mvp,         XMMatrixTranspose(modelMat * view * proj));
                XMStoreFloat4x4(&cb.modelMatrix, XMMatrixTranspose(modelMat));

                // 修正：僅儲存 M^-1（不外加 Transpose）
                // HLSL 端改為 mul((float3x3)normalMatrix, v.normal)，
                // 兩者合起來等於對法線套用 (M^-1)^T，即正確的 inverse-transpose。
                XMStoreFloat4x4(&cb.normalMatrix, XMMatrixInverse(nullptr, modelMat));

                XMStoreFloat3(&cb.lightDir, XMVector3Normalize(forward));
                cb.baseColor = { 0.8f, 0.6f, 0.4f, 1.0f };
                cb.cameraPos = m_cameraPos;
                m_cmdList->SetGraphicsRoot32BitConstants(0, sizeof(SceneConstants) / 4, &cb, 0);

                for (int subIdx : node.subMeshIndices) {
                    const auto& sub = mesh->subMeshes[subIdx];
                    if (sub.isTransparent != drawTransparent) continue;
                    int matIdx = (sub.materialIndex >= 0 && sub.materialIndex < (int)mesh->texturePaths.size())
                        ? sub.materialIndex : 0;
                    CD3DX12_GPU_DESCRIPTOR_HANDLE srvHandle(
                        inst.srvHeap->GetGPUDescriptorHandleForHeapStart(),
                        matIdx * 2, m_srvDescriptorSize);
                    m_cmdList->SetGraphicsRootDescriptorTable(1, srvHandle);
                    m_cmdList->DrawIndexedInstanced(sub.indexCount, 1, sub.indexOffset, 0, 0);
                    currentDrawCalls++;
                }
            }
        };

        m_cmdList->SetPipelineState(m_psoOpaque.Get());
        drawPass(false);
        m_cmdList->SetPipelineState(m_psoTransparent.Get());
        drawPass(true);

        totalVerts += (int)mesh->vertices.size();
        totalPolys += (int)mesh->indices.size() / 3;
    }

    m_statVertices.store(totalVerts,  std::memory_order_relaxed);
    m_statPolygons.store(totalPolys,  std::memory_order_relaxed);
    m_statDrawCalls.store(currentDrawCalls, std::memory_order_relaxed);

    barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        m_renderTargets[m_frameIndex].Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    m_cmdList->ResourceBarrier(1, &barrier);
    m_cmdList->Close();

    ID3D12CommandList* lists[] = { m_cmdList.Get() };
    m_cmdQueue->ExecuteCommandLists(1, lists);
    m_renderMutex.unlock();
    m_swapChain->Present(1, 0);

    m_currentFenceValue++;
    m_cmdQueue->Signal(m_fence.Get(), m_currentFenceValue);
    m_fenceValues[m_frameIndex] = m_currentFenceValue;
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    if (m_fence->GetCompletedValue() < m_fenceValues[m_frameIndex]) {
        m_fence->SetEventOnCompletion(m_fenceValues[m_frameIndex], m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, INFINITE);
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
        while (tempW > 1 || tempH > 1) { data.mipLevels++; tempW = (std::max)(1u, tempW/2); tempH = (std::max)(1u, tempH/2); }
        data.mipData.resize(data.mipLevels);
        data.mipData[0].assign(pixels, pixels + (data.width * data.height * 4));
        UINT currW = data.width, currH = data.height;
        for (UINT m = 1; m < data.mipLevels; ++m) {
            UINT prevW = currW, prevH = currH;
            currW = (std::max)(1u, currW/2); currH = (std::max)(1u, currH/2);
            data.mipData[m].resize(currW * currH * 4);
            const uint8_t* src = data.mipData[m-1].data();
            uint8_t* dst = data.mipData[m].data();
            for (UINT y = 0; y < currH; ++y)
                for (UINT x = 0; x < currW; ++x) {
                    UINT sx=x*2, sy=y*2, sx1=(std::min)(sx+1,prevW-1), sy1=(std::min)(sy+1,prevH-1);
                    for (int c = 0; c < 4; ++c)
                        dst[(y*currW+x)*4+c] = (src[(sy*prevW+sx)*4+c]+src[(sy*prevW+sx1)*4+c]+src[(sy1*prevW+sx)*4+c]+src[(sy1*prevW+sx1)*4+c])/4;
                }
        }
        if (pixels != (stbi_uc*)&defaultColor) stbi_image_free(pixels);
        return data;
    };

    UINT numMaterials = (std::max)(1, (int)mesh->texturePaths.size());
    std::vector<TextureCpuData> baseColors(numMaterials), metallicRoughness(numMaterials);
    for (size_t i = 0; i < numMaterials; i++) {
        baseColors[i]        = PrepareTextureData(mesh->texturePaths[i], 0xFFFFFFFF);
        metallicRoughness[i] = PrepareTextureData(
            i < mesh->metallicRoughnessPaths.size() ? mesh->metallicRoughnessPaths[i] : "",
            0xFF00FF00);
    }

    std::lock_guard<std::mutex> lock(m_renderMutex);
    WaitForGpu();

    MeshInstance inst;
    inst.meshId = meshId;
    inst.mesh   = mesh;

    auto uploadHeap  = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    auto defaultHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

    UINT64 vbSize = mesh->vertices.size() * sizeof(Vertex);
    UINT64 ibSize = mesh->indices.size()  * sizeof(uint32_t);

    auto vbDesc = CD3DX12_RESOURCE_DESC::Buffer(vbSize);
    m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &vbDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&mesh->vertexBuffer));
    m_device->CreateCommittedResource(&uploadHeap,  D3D12_HEAP_FLAG_NONE, &vbDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&inst.vbUpload));
    void* mapped;
    inst.vbUpload->Map(0, nullptr, &mapped); memcpy(mapped, mesh->vertices.data(), vbSize); inst.vbUpload->Unmap(0, nullptr);

    auto ibDesc = CD3DX12_RESOURCE_DESC::Buffer(ibSize);
    m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &ibDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&mesh->indexBuffer));
    m_device->CreateCommittedResource(&uploadHeap,  D3D12_HEAP_FLAG_NONE, &ibDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&inst.ibUpload));
    inst.ibUpload->Map(0, nullptr, &mapped); memcpy(mapped, mesh->indices.data(), ibSize); inst.ibUpload->Unmap(0, nullptr);

    m_cmdAllocators[0]->Reset();
    m_cmdList->Reset(m_cmdAllocators[0].Get(), nullptr);
    m_cmdList->CopyResource(mesh->vertexBuffer.Get(), inst.vbUpload.Get());
    m_cmdList->CopyResource(mesh->indexBuffer.Get(),  inst.ibUpload.Get());
    D3D12_RESOURCE_BARRIER barriers[2] = {
        CD3DX12_RESOURCE_BARRIER::Transition(mesh->vertexBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER),
        CD3DX12_RESOURCE_BARRIER::Transition(mesh->indexBuffer.Get(),  D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDEX_BUFFER),
    };
    m_cmdList->ResourceBarrier(2, barriers);
    m_cmdList->Close();
    ID3D12CommandList* lists[] = { m_cmdList.Get() };
    m_cmdQueue->ExecuteCommandLists(1, lists);
    WaitForGpu();

    mesh->vbView = { mesh->vertexBuffer->GetGPUVirtualAddress(), (UINT)vbSize, sizeof(Vertex) };
    mesh->ibView = { mesh->indexBuffer->GetGPUVirtualAddress(),  (UINT)ibSize, DXGI_FORMAT_R32_UINT };

    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.NumDescriptors = numMaterials * 2;
    srvHeapDesc.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    CHECK(m_device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&inst.srvHeap)));

    m_cmdAllocators[0]->Reset();
    m_cmdList->Reset(m_cmdAllocators[0].Get(), nullptr);
    CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle(inst.srvHeap->GetCPUDescriptorHandleForHeapStart());
    std::vector<ComPtr<ID3D12Resource>> uploadBuffers;

    auto UploadToGPU = [&](const TextureCpuData& cpuData) {
        inst.textures.emplace_back();
        uploadBuffers.emplace_back();
        auto texDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, cpuData.width, cpuData.height, 1, (UINT16)cpuData.mipLevels);
        auto dh = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        m_device->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &texDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&inst.textures.back()));
        UINT64 uploadSize = 0;
        m_device->GetCopyableFootprints(&texDesc, 0, cpuData.mipLevels, 0, nullptr, nullptr, nullptr, &uploadSize);
        auto uh = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        auto bufDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadSize);
        m_device->CreateCommittedResource(&uh, D3D12_HEAP_FLAG_NONE, &bufDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadBuffers.back()));
        std::vector<D3D12_SUBRESOURCE_DATA> subresources(cpuData.mipLevels);
        for (UINT m = 0; m < cpuData.mipLevels; ++m) {
            UINT cw = (std::max)(1u, (UINT)(cpuData.width >> m));
            UINT ch = (std::max)(1u, (UINT)(cpuData.height >> m));
            subresources[m] = { cpuData.mipData[m].data(), (LONG_PTR)(cw*4), (LONG_PTR)(cw*4*ch) };
        }
        UpdateSubresources(m_cmdList.Get(), inst.textures.back().Get(), uploadBuffers.back().Get(), 0, 0, cpuData.mipLevels, subresources.data());
        auto bar = CD3DX12_RESOURCE_BARRIER::Transition(inst.textures.back().Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_cmdList->ResourceBarrier(1, &bar);
        D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Format = texDesc.Format;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = cpuData.mipLevels;
        m_device->CreateShaderResourceView(inst.textures.back().Get(), &sv, srvHandle);
        srvHandle.Offset(1, m_srvDescriptorSize);
    };

    for (size_t i = 0; i < numMaterials; i++) {
        UploadToGPU(baseColors[i]);
        UploadToGPU(metallicRoughness[i]);
    }
    m_cmdList->Close();
    ID3D12CommandList* cmds[] = { m_cmdList.Get() };
    m_cmdQueue->ExecuteCommandLists(1, cmds);
    WaitForGpu();

    m_meshes.push_back(std::move(inst));
}

// ---------------------------------------------------------------------------
// Camera / Resize / WaitForGpu / CreateXxx
// ---------------------------------------------------------------------------
void Renderer::SetCameraTransform(float px, float py, float pz, float pitch, float yaw) {
    std::lock_guard<std::mutex> lock(m_renderMutex);
    m_cameraPos = { px, py, pz };
    m_pitch = pitch;
    m_yaw   = yaw;
}

void Renderer::WaitForGpu() {
    m_currentFenceValue++;
    m_cmdQueue->Signal(m_fence.Get(), m_currentFenceValue);
    if (m_fence->GetCompletedValue() < m_currentFenceValue) {
        m_fence->SetEventOnCompletion(m_currentFenceValue, m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
}

void Renderer::Resize(int width, int height, float scale) {
    std::lock_guard<std::mutex> lock(m_renderMutex);
    if (width <= 0 || height <= 0) return;
    DXGI_MATRIX_3X2_F inverseScale = { 1.0f/scale, 0.0f, 0.0f, 1.0f/scale, 0.0f, 0.0f };
    m_swapChain->SetMatrixTransform(&inverseScale);
    if (width == m_width && height == m_height) return;
    WaitForGpu();
    for (auto& rt : m_renderTargets) rt.Reset();
    m_depthStencil.Reset();
    m_swapChain->ResizeBuffers(FRAME_COUNT, (UINT)width, (UINT)height, DXGI_FORMAT_R8G8B8A8_UNORM, 0);
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    m_width = width; m_height = height;
    CreateRTV();
    CreateDSV();
}

void Renderer::CreateDeviceAndQueue() {
#ifdef _DEBUG
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
#endif
    ComPtr<IDXGIFactory7> factory;
    CHECK(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
    CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device)));
    D3D12_COMMAND_QUEUE_DESC qDesc = {};
    qDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    CHECK(m_device->CreateCommandQueue(&qDesc, IID_PPV_ARGS(&m_cmdQueue)));
}

void Renderer::CreateSwapChain(IUnknown* panelUnknown, int width, int height) {
    ComPtr<IDXGIFactory7> factory;
    CHECK(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = (UINT)width; desc.Height = (UINT)height;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferCount = FRAME_COUNT;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.SampleDesc = { 1, 0 };
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    ComPtr<IDXGISwapChain1> sc1;
    CHECK(factory->CreateSwapChainForComposition(m_cmdQueue.Get(), &desc, nullptr, &sc1));
    CHECK(sc1.As(&m_swapChain));
    ComPtr<ISwapChainPanelNative> panelNative;
    CHECK(panelUnknown->QueryInterface(IID_PPV_ARGS(&panelNative)));
    CHECK(panelNative->SetSwapChain(m_swapChain.Get()));
}

void Renderer::CreateRTV() {
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.NumDescriptors = FRAME_COUNT;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    CHECK(m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_rtvHeap)));
    m_rtvDescSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_rtvHeap->GetCPUDescriptorHandleForHeapStart());
    for (UINT i = 0; i < FRAME_COUNT; i++) {
        CHECK(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_renderTargets[i])));
        m_device->CreateRenderTargetView(m_renderTargets[i].Get(), nullptr, rtvHandle);
        rtvHandle.Offset(1, m_rtvDescSize);
    }
}

void Renderer::CreateDSV() {
    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
    dsvHeapDesc.NumDescriptors = 1;
    dsvHeapDesc.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    CHECK(m_device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&m_dsvHeap)));
    D3D12_CLEAR_VALUE optClear = {};
    optClear.Format = DXGI_FORMAT_D32_FLOAT;
    optClear.DepthStencil.Depth = 1.0f;
    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    auto texDesc   = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_D32_FLOAT, m_width, m_height, 1, 0, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    CHECK(m_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &texDesc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &optClear, IID_PPV_ARGS(&m_depthStencil)));
    m_device->CreateDepthStencilView(m_depthStencil.Get(), nullptr, m_dsvHeap->GetCPUDescriptorHandleForHeapStart());
}

void Renderer::CreateRootSignatureAndPSO() {
    CD3DX12_DESCRIPTOR_RANGE1 srvRange;
    srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0);
    CD3DX12_ROOT_PARAMETER1 rootParameters[2];
    rootParameters[0].InitAsConstants(sizeof(SceneConstants) / 4, 0);
    rootParameters[1].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_PIXEL);
    CD3DX12_STATIC_SAMPLER_DESC sampler(0, D3D12_FILTER_ANISOTROPIC);
    sampler.MaxAnisotropy = 16;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rsDesc;
    rsDesc.Init_1_1(2, rootParameters, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
    ComPtr<ID3DBlob> sigBlob, errBlob;
    CHECK(D3DX12SerializeVersionedRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1_1, &sigBlob, &errBlob));
    CHECK(m_device->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&m_rootSig)));
    ComPtr<ID3DBlob> vsBlob, psBlob;
    CHECK(D3DReadFileToBlob(GetShaderPath(L"BaseColor_VS.cso").c_str(), &vsBlob));
    CHECK(D3DReadFileToBlob(GetShaderPath(L"BaseColor_PS.cso").c_str(), &psBlob));
    D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout        = { layout, _countof(layout) };
    psoDesc.pRootSignature     = m_rootSig.Get();
    psoDesc.VS                 = CD3DX12_SHADER_BYTECODE(vsBlob.Get());
    psoDesc.PS                 = CD3DX12_SHADER_BYTECODE(psBlob.Get());
    psoDesc.RasterizerState    = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.DepthStencilState  = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    psoDesc.SampleMask         = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets   = 1;
    psoDesc.RTVFormats[0]      = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.DSVFormat          = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc.Count   = 1;
    psoDesc.BlendState         = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    CHECK(m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_psoOpaque)));
    D3D12_BLEND_DESC blendDesc = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    blendDesc.RenderTarget[0].BlendEnable    = TRUE;
    blendDesc.RenderTarget[0].SrcBlend       = D3D12_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend      = D3D12_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp        = D3D12_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha  = D3D12_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    blendDesc.RenderTarget[0].BlendOpAlpha   = D3D12_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.BlendState = blendDesc;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    CHECK(m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_psoTransparent)));
}
