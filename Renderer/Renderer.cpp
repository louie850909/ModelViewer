#include "pch.h"
#include "Renderer.h"
#include <stdexcept>
#include <filesystem>
#include <string>
#include <algorithm>
#include <stb_image.h>

#define CHECK(hr) if(FAILED(hr)) throw std::runtime_error("DX12 HRESULT failed")

// 取得與執行檔同目錄的 Shader 絕對路徑
std::wstring GetShaderPath(const std::wstring & filename) {
    wchar_t buffer[MAX_PATH];
    // 傳入 nullptr 代表取得目前啟動的 .exe 絕對路徑
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);

    std::filesystem::path exePath(buffer);
    // 組合出絕對路徑： [Exe所在目錄] \ shaders \ [檔名]
    return (exePath.parent_path() / L"shaders" / filename).wstring();
}

bool Renderer::Init(IUnknown* panelUnknown, int width, int height) {
    m_width = width; m_height = height;
    try {
        CreateDeviceAndQueue();        // 1. Device + CommandQueue
        CreateSwapChain(panelUnknown, width, height); // 2. SwapChain 綁定 Panel
        CreateRTV();                   // 3. RenderTargetView heap
        CreateDSV();

        // 4. CommandAllocator + CommandList
        for (UINT i = 0; i < FRAME_COUNT; i++) {
            CHECK(m_device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(&m_cmdAllocators[i])));
        }
        CHECK(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            m_cmdAllocators[0].Get(), nullptr, IID_PPV_ARGS(&m_cmdList)));
        CHECK(m_cmdList->Close());

        // 5. Fence
        CHECK(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
            IID_PPV_ARGS(&m_fence)));
        m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        m_currentFenceValue = 0;
        m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

        CreateRootSignatureAndPSO();   // 6. RootSignature + PSO + CBuffer
        //    （需要 Device 已建立，故放最後）

        m_lastFrameTime = std::chrono::high_resolution_clock::now(); // 初始化計時器
        return true;
    }
    catch (...) { return false; }
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
    desc.Width = (UINT)width;
    desc.Height = (UINT)height;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferCount = FRAME_COUNT;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.SampleDesc = { 1, 0 };
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED; // Composition 必要

    ComPtr<IDXGISwapChain1> sc1;
    // CreateSwapChainForComposition，不是 ForHwnd！
    CHECK(factory->CreateSwapChainForComposition(m_cmdQueue.Get(), &desc, nullptr, &sc1));
    CHECK(sc1.As(&m_swapChain));

    // 綁定到 SwapChainPanel
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

void Renderer::RenderFrame() {
    // 計算 Frame Time (毫秒)
    auto now = std::chrono::high_resolution_clock::now();
    std::chrono::duration<float, std::milli> timeSpan = now - m_lastFrameTime;
    m_lastFrameTime = now;
    m_statFrameTime.store(timeSpan.count(), std::memory_order_relaxed);

    int currentDrawCalls = 0; // 用來累加當前幀的 DrawCall

    // 手動上鎖：保護 m_yaw, m_pitch, m_mesh 等變數的讀取
    m_renderMutex.lock();

    auto& alloc = m_cmdAllocators[m_frameIndex];
    alloc->Reset();
    m_cmdList->Reset(alloc.Get(), nullptr);

    // Barrier: Present → RenderTarget
    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        m_renderTargets[m_frameIndex].Get(),
        D3D12_RESOURCE_STATE_PRESENT,
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    m_cmdList->ResourceBarrier(1, &barrier);

    // Clear
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtv(
        m_rtvHeap->GetCPUDescriptorHandleForHeapStart(),
        (INT)m_frameIndex, m_rtvDescSize);
    float clearColor[] = { 0.2f, 0.2f, 0.2f, 1.0f };
    m_cmdList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);

    // 取得 DSV handle 並清空深度
    CD3DX12_CPU_DESCRIPTOR_HANDLE dsv(m_dsvHeap->GetCPUDescriptorHandleForHeapStart());
    m_cmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    if (m_mesh) {
        using namespace DirectX;

        // 1. 計算所有節點的世界矩陣 (Global Transforms)
        std::vector<XMMATRIX> globalTransforms(m_mesh->nodes.size());
        for (size_t i = 0; i < m_mesh->nodes.size(); ++i) {
            const auto& node = m_mesh->nodes[i];

            // Local Transform = Scale * Rotation * Translation
            XMMATRIX local = XMMatrixScaling(node.s[0], node.s[1], node.s[2]) *
                XMMatrixRotationQuaternion(XMVectorSet(node.r[0], node.r[1], node.r[2], node.r[3])) *
                XMMatrixTranslation(node.t[0], node.t[1], node.t[2]);

            // 如果有父節點，就乘上父節點的世界矩陣 (階層繼承)
            if (node.parentIndex >= 0) {
                globalTransforms[i] = local * globalTransforms[node.parentIndex];
            }
            else {
                globalTransforms[i] = local;
            }
        }

        // 2. 計算攝影機與 View Projection
        XMMATRIX rotation = XMMatrixRotationRollPitchYaw(m_pitch, m_yaw, 0.0f);
        XMVECTOR eye = XMLoadFloat3(&m_cameraPos);
        XMVECTOR forward = XMVector3TransformNormal(XMVectorSet(0, 0, 1, 0), rotation);
        XMVECTOR up = XMVectorSet(0, 1, 0, 0);
        XMMATRIX view = XMMatrixLookAtLH(eye, eye + forward, up);
        XMMATRIX proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(45.f), (float)m_width / m_height, 0.1f, 5000.f);

        // 3. 基本渲染設定
        D3D12_VIEWPORT vp = { 0,0,(float)m_width,(float)m_height,0,1 };
        D3D12_RECT     sc = { 0,0,m_width,m_height };
        m_cmdList->RSSetViewports(1, &vp);
        m_cmdList->RSSetScissorRects(1, &sc);
        m_cmdList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        m_cmdList->SetGraphicsRootSignature(m_rootSig.Get());

        m_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_cmdList->IASetVertexBuffers(0, 1, &m_mesh->vbView);
        m_cmdList->IASetIndexBuffer(&m_mesh->ibView);

        ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
        m_cmdList->SetDescriptorHeaps(1, heaps);

        // 4. 定義一個 Lambda 來處理不透明與半透明的繪製
        auto drawPass = [&](bool drawTransparent) {
            for (size_t n = 0; n < m_mesh->nodes.size(); ++n) {
                const auto& node = m_mesh->nodes[n];
                if (node.subMeshIndices.empty()) continue; // 沒東西要畫就跳過

                // 組合「這個節點」專屬的 MVP 矩陣
                SceneConstants cb = {};
                XMStoreFloat4x4(&cb.mvp, XMMatrixTranspose(globalTransforms[n] * view * proj));
                XMStoreFloat3(&cb.lightDir, XMVector3Normalize(forward));
                XMStoreFloat4x4(&cb.normalMatrix, XMMatrixTranspose(XMMatrixInverse(nullptr, globalTransforms[n])));
                cb.baseColor = { 0.8f, 0.6f, 0.4f, 1.0f };
                cb.cameraPos = m_cameraPos;

                // 使用 Root Constants 直接把 44 個 float (176 bytes) 送進 GPU 管線
                m_cmdList->SetGraphicsRoot32BitConstants(0, sizeof(SceneConstants) / 4, &cb, 0);

                // 繪製這個節點底下的所有 SubMesh
                for (int subIdx : node.subMeshIndices) {
                    const auto& sub = m_mesh->subMeshes[subIdx];
                    if (sub.isTransparent != drawTransparent) continue;

                    int matIdx = sub.materialIndex;
                    if (matIdx < 0 || matIdx >= m_mesh->texturePaths.size()) matIdx = 0;

                    CD3DX12_GPU_DESCRIPTOR_HANDLE srvGpuHandle(m_srvHeap->GetGPUDescriptorHandleForHeapStart(), matIdx * 2, m_srvDescriptorSize);
                    m_cmdList->SetGraphicsRootDescriptorTable(1, srvGpuHandle);
                    m_cmdList->DrawIndexedInstanced(sub.indexCount, 1, sub.indexOffset, 0, 0);
                    currentDrawCalls++;
                }
            }
            };

        // 5. 實際執行繪製
        m_cmdList->SetPipelineState(m_psoOpaque.Get());
        drawPass(false); // 先畫不透明物件

        m_cmdList->SetPipelineState(m_psoTransparent.Get());
        drawPass(true);  // 再畫半透明物件

        m_statVertices.store((int)m_mesh->vertices.size(), std::memory_order_relaxed);
        m_statPolygons.store((int)m_mesh->indices.size() / 3, std::memory_order_relaxed);
    }
    else
    {
        m_statVertices.store(0, std::memory_order_relaxed);
        m_statPolygons.store(0, std::memory_order_relaxed);
    }

    m_statDrawCalls.store(currentDrawCalls, std::memory_order_relaxed); // 寫入總 DrawCall

    // Barrier: RenderTarget → Present
    barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        m_renderTargets[m_frameIndex].Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PRESENT);
    m_cmdList->ResourceBarrier(1, &barrier);
    m_cmdList->Close();

    // 必須先 Execute，才能 Unlock！
    ID3D12CommandList* lists[] = { m_cmdList.Get() };
    m_cmdQueue->ExecuteCommandLists(1, lists);

    // 指令已經安全送進 Queue 了，現在解鎖是安全的
    m_renderMutex.unlock();
    m_swapChain->Present(1, 0);

    //1. 推進全域 Fence 計數器，並要求 GPU 執行到這裡時發出信號
    m_currentFenceValue++;
    m_cmdQueue->Signal(m_fence.Get(), m_currentFenceValue);

    //2. 紀錄「當前這一個 BackBuffer」對應的 Fence 值
    m_fenceValues[m_frameIndex] = m_currentFenceValue;

    //3. 切換到下一個 Frame Index
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    //4. 檢查「下一個要用的 Frame」是否已經被 GPU 處理完畢？若還沒，就等它！
    if (m_fence->GetCompletedValue() < m_fenceValues[m_frameIndex]) {
        m_fence->SetEventOnCompletion(m_fenceValues[m_frameIndex], m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
}

void Renderer::Resize(int width, int height) {
    std::lock_guard<std::mutex> lock(m_renderMutex);

    // 加入 0 尺寸防呆，避免除以零或建立無效材質
    if (width <= 0 || height <= 0) return;
    if (width == m_width && height == m_height) return;
    WaitForGpu();
    for (auto& rt : m_renderTargets) rt.Reset();

    // 釋放舊的 Depth Buffer 
    m_depthStencil.Reset();

    m_swapChain->ResizeBuffers(FRAME_COUNT, (UINT)width, (UINT)height,
        DXGI_FORMAT_R8G8B8A8_UNORM, 0);
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    m_width = width; m_height = height;
    CreateRTV();
    CreateDSV();
}

void Renderer::SetCameraTransform(float px, float py, float pz, float pitch, float yaw) {
    std::lock_guard<std::mutex> lock(m_renderMutex);
    m_cameraPos = { px, py, pz };
    m_pitch = pitch;
    m_yaw = yaw;
}

void Renderer::WaitForGpu() {
    // 使用單一計數器
    m_currentFenceValue++;
    m_cmdQueue->Signal(m_fence.Get(), m_currentFenceValue);
    if (m_fence->GetCompletedValue() < m_currentFenceValue) {
        m_fence->SetEventOnCompletion(m_currentFenceValue, m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
}

void Renderer::Shutdown() {
    WaitForGpu();
    CloseHandle(m_fenceEvent);
}

void Renderer::CreateRootSignatureAndPSO() {
    // --- 建立 Root Signature ---
    // 1. 定義貼圖的位置 (t0)
    CD3DX12_DESCRIPTOR_RANGE1 srvRange;
    srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0);

    // 2. 設定 Root Parameters (0 是 MVP 矩陣，1 是貼圖陣列)
    CD3DX12_ROOT_PARAMETER1 rootParameters[2];
    // 44 代表 SceneConstants 結構的大小除以 4 (176 bytes / 4 = 44 個 32-bit 數值)
    rootParameters[0].InitAsConstants(44, 0);
    rootParameters[1].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_PIXEL);

    // 3. 建立一個預設的靜態採樣器 (s0)
    CD3DX12_STATIC_SAMPLER_DESC sampler(0, D3D12_FILTER_ANISOTROPIC);
    sampler.MaxAnisotropy = 16;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // 4. 打包簽名 (宣告為 rsDesc 以符合您下方的序列化程式碼)
    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rsDesc;
    rsDesc.Init_1_1(2, rootParameters, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    // 5. 序列化
    ComPtr<ID3DBlob> sigBlob, errBlob;
    CHECK(D3DX12SerializeVersionedRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1_1, &sigBlob, &errBlob));
    CHECK(m_device->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&m_rootSig)));

    // 讀取編譯好的 shader（cso 檔案）
    ComPtr<ID3DBlob> vsBlob, psBlob;
    std::wstring vsPath = GetShaderPath(L"BaseColor_VS.cso");
    std::wstring psPath = GetShaderPath(L"BaseColor_PS.cso");

    CHECK(D3DReadFileToBlob(vsPath.c_str(), &vsBlob));
    CHECK(D3DReadFileToBlob(psPath.c_str(), &psBlob));

    // Input Layout
    D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    // PSO
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = { layout, _countof(layout) };
    psoDesc.pRootSignature = m_rootSig.Get();
    psoDesc.VS = CD3DX12_SHADER_BYTECODE(vsBlob.Get());
    psoDesc.PS = CD3DX12_SHADER_BYTECODE(psBlob.Get());

	// 大部分狀態使用預設值，只有幾個需要特別設定
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);

	// 關鍵：不透明和半透明物件都要開啟 Depth Test，才能正確遮擋
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc.Count = 1;

    // ==========================================
    // 1. 建立「不透明 (Opaque)」PSO
    // ==========================================
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT); // 關閉混合
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL; // 開啟深度寫入
    CHECK(m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_psoOpaque)));

    // ==========================================
    // 2. 建立「半透明 (Transparent)」PSO
    // ==========================================
    D3D12_BLEND_DESC blendDesc = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    psoDesc.BlendState = blendDesc;

    // 關鍵：半透明物件絕對不能寫入 Depth Buffer！否則會遮擋後方的半透明物
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;

    CHECK(m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_psoTransparent)));
}

void Renderer::UploadMeshToGpu(std::shared_ptr<Mesh> mesh) {
    // CPU 處理好的圖片像素
    struct TextureCpuData {
        int width, height;
        UINT mipLevels;
        std::vector<std::vector<uint8_t>> mipData;
    };

    // ==========================================
    // [CPU 階段] 無鎖背景處理：解碼貼圖與生成 Mipmap
    // 在這裡 Render Thread 依然可以繼續畫舊的模型，完全不卡頓！
    // ==========================================

    // 定義一個 Lambda 專門處理純 CPU 的讀圖與降取樣運算
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
        while (tempW > 1 || tempH > 1) {
            data.mipLevels++;
            tempW = (std::max)(1u, tempW / 2);
            tempH = (std::max)(1u, tempH / 2);
        }

        data.mipData.resize(data.mipLevels);
        data.mipData[0].assign(pixels, pixels + (data.width * data.height * 4));

        UINT currW = data.width, currH = data.height;
        for (UINT m = 1; m < data.mipLevels; ++m) {
            UINT prevW = currW, prevH = currH;
            currW = (std::max)(1u, currW / 2);
            currH = (std::max)(1u, currH / 2);
            data.mipData[m].resize(currW * currH * 4);
            const uint8_t* src = data.mipData[m - 1].data();
            uint8_t* dst = data.mipData[m].data();
            for (UINT y = 0; y < currH; ++y) {
                for (UINT x = 0; x < currW; ++x) {
                    UINT sx = x * 2, sy = y * 2;
                    UINT sx1 = (std::min)(sx + 1, prevW - 1);
                    UINT sy1 = (std::min)(sy + 1, prevH - 1);
                    for (int c = 0; c < 4; ++c) {
                        int p00 = src[(sy * prevW + sx) * 4 + c];
                        int p10 = src[(sy * prevW + sx1) * 4 + c];
                        int p01 = src[(sy1 * prevW + sx) * 4 + c];
                        int p11 = src[(sy1 * prevW + sx1) * 4 + c];
                        dst[(y * currW + x) * 4 + c] = (p00 + p10 + p01 + p11) / 4;
                    }
                }
            }
        }
        if (pixels != (stbi_uc*)&defaultColor) stbi_image_free(pixels);
        return data;
        };

    UINT numMaterials = (std::max)(1, (int)mesh->texturePaths.size());
    std::vector<TextureCpuData> baseColors(numMaterials);
    std::vector<TextureCpuData> metallicRoughness(numMaterials);

    // 平行或依序在 CPU 準備好所有圖片像素
    for (size_t i = 0; i < numMaterials; i++) {
        baseColors[i] = PrepareTextureData(mesh->texturePaths[i], 0xFFFFFFFF);
        metallicRoughness[i] = PrepareTextureData(mesh->metallicRoughnessPaths[i], 0xFF00FF00);
    }

    // ==========================================
    // [GPU 階段] 上鎖並交給 DX12 建立資源
    // 這裡只剩下單純的記憶體拷貝與 DX12 API 呼叫，速度極快
    // ==========================================
    std::lock_guard<std::mutex> lock(m_renderMutex);
    WaitForGpu();

    m_mesh = mesh;

    auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    auto defaultHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

    UINT64 vbSize = mesh->vertices.size() * sizeof(Vertex);
    UINT64 ibSize = mesh->indices.size() * sizeof(uint32_t);

    // Vertex Buffer
    auto vbDesc = CD3DX12_RESOURCE_DESC::Buffer(vbSize);
    m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE,
        &vbDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&mesh->vertexBuffer));
    m_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE,
        &vbDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_vbUpload));

    void* mapped;
    m_vbUpload->Map(0, nullptr, &mapped);
    memcpy(mapped, mesh->vertices.data(), vbSize);
    m_vbUpload->Unmap(0, nullptr);

    // Index Buffer
    auto ibDesc = CD3DX12_RESOURCE_DESC::Buffer(ibSize);
    m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE,
        &ibDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&mesh->indexBuffer));
    m_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE,
        &ibDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_ibUpload));

    m_ibUpload->Map(0, nullptr, &mapped);
    memcpy(mapped, mesh->indices.data(), ibSize);
    m_ibUpload->Unmap(0, nullptr);

    // 用 Command List 執行 Copy
    m_cmdAllocators[0]->Reset();
    m_cmdList->Reset(m_cmdAllocators[0].Get(), nullptr);
    m_cmdList->CopyResource(mesh->vertexBuffer.Get(), m_vbUpload.Get());
    m_cmdList->CopyResource(mesh->indexBuffer.Get(), m_ibUpload.Get());

    // Barrier: CopyDest → VertexBuffer / IndexBuffer
    D3D12_RESOURCE_BARRIER barriers[2] = {
        CD3DX12_RESOURCE_BARRIER::Transition(mesh->vertexBuffer.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER),
        CD3DX12_RESOURCE_BARRIER::Transition(mesh->indexBuffer.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDEX_BUFFER),
    };
    m_cmdList->ResourceBarrier(2, barriers);
    m_cmdList->Close();

    ID3D12CommandList* lists[] = { m_cmdList.Get() };
    m_cmdQueue->ExecuteCommandLists(1, lists);
    WaitForGpu(); // 等上傳完成

    // Views
    mesh->vbView = { mesh->vertexBuffer->GetGPUVirtualAddress(), (UINT)vbSize, sizeof(Vertex) };
    mesh->ibView = { mesh->indexBuffer->GetGPUVirtualAddress(), (UINT)ibSize, DXGI_FORMAT_R32_UINT };

    // 取得 SRV Descriptor 的大小 (DX12 規定必須動態查詢)
    m_srvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    // 每個材質需要 2 個位置 (BaseColor + MetallicRoughness)
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.NumDescriptors = numMaterials * 2;
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE; // 必須設定為 Shader 可見
    if (FAILED(m_device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&m_srvHeap)))) {
        throw std::runtime_error("Failed to create SRV Heap");
    }

    m_textures.clear();
    std::vector<ComPtr<ID3D12Resource>> uploadBuffers; // 暫存區

    m_cmdAllocators[0]->Reset();
    m_cmdList->Reset(m_cmdAllocators[0].Get(), nullptr);
    CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle(m_srvHeap->GetCPUDescriptorHandleForHeapStart());

    // 定義新的 Lambda，這時候只負責把剛才算好的 TextureCpuData 餵給 GPU
    auto UploadToGPU = [&](const TextureCpuData& cpuData) {
        m_textures.emplace_back();
        uploadBuffers.emplace_back();

        auto texDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, cpuData.width, cpuData.height, 1, (UINT16)cpuData.mipLevels);
        auto defaultHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &texDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_textures.back()));

        UINT64 uploadSize = 0;
        m_device->GetCopyableFootprints(&texDesc, 0, cpuData.mipLevels, 0, nullptr, nullptr, nullptr, &uploadSize);
        auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        auto bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadSize);
        m_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadBuffers.back()));

        std::vector<D3D12_SUBRESOURCE_DATA> subresources(cpuData.mipLevels);
        for (UINT m = 0; m < cpuData.mipLevels; ++m) {
            subresources[m].pData = cpuData.mipData[m].data();
            // 注意這裡要根據當下 mip 層級的寬度計算 RowPitch
            UINT currentW = (std::max)(1u, (UINT)(cpuData.width >> m));
            UINT currentH = (std::max)(1u, (UINT)(cpuData.height >> m));
            subresources[m].RowPitch = currentW * 4;
            subresources[m].SlicePitch = subresources[m].RowPitch * currentH;
        }

        UpdateSubresources(m_cmdList.Get(), m_textures.back().Get(), uploadBuffers.back().Get(), 0, 0, cpuData.mipLevels, subresources.data());

        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_textures.back().Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_cmdList->ResourceBarrier(1, &barrier);

        D3D12_SHADER_RESOURCE_VIEW_DESC srvViewDesc = {};
        srvViewDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvViewDesc.Format = texDesc.Format;
        srvViewDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvViewDesc.Texture2D.MipLevels = cpuData.mipLevels;
        m_device->CreateShaderResourceView(m_textures.back().Get(), &srvViewDesc, srvHandle);
        srvHandle.Offset(1, m_srvDescriptorSize);
        };

    // 依序上傳 BaseColor 與 MetallicRoughness
    for (size_t i = 0; i < numMaterials; i++) {
        UploadToGPU(baseColors[i]);
        UploadToGPU(metallicRoughness[i]);
    }

    m_cmdList->Close();
    ID3D12CommandList* cmds[] = { m_cmdList.Get() };
    m_cmdQueue->ExecuteCommandLists(1, cmds);
    WaitForGpu();
}

void Renderer::CreateDSV() {
    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
    dsvHeapDesc.NumDescriptors = 1;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    CHECK(m_device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&m_dsvHeap)));

    D3D12_CLEAR_VALUE optClear = {};
    optClear.Format = DXGI_FORMAT_D32_FLOAT;
    optClear.DepthStencil.Depth = 1.0f;
    optClear.DepthStencil.Stencil = 0;

    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    auto texDesc = CD3DX12_RESOURCE_DESC::Tex2D(
        DXGI_FORMAT_D32_FLOAT, m_width, m_height, 1, 0, 1, 0,
        D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);

    CHECK(m_device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE,
        &texDesc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
        &optClear, IID_PPV_ARGS(&m_depthStencil)));

    m_device->CreateDepthStencilView(m_depthStencil.Get(), nullptr, m_dsvHeap->GetCPUDescriptorHandleForHeapStart());
}