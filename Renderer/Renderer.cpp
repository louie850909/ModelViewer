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

    if (m_mesh && m_pso) {
        using namespace DirectX;

        // 模型保持靜止不動
        XMMATRIX model = XMMatrixIdentity();

        // 2. 計算攝影機位置
        XMMATRIX rotation = XMMatrixRotationRollPitchYaw(m_pitch, m_yaw, 0.0f);
        XMVECTOR eye = XMLoadFloat3(&m_cameraPos);
        XMVECTOR forward = XMVector3TransformNormal(XMVectorSet(0, 0, 1, 0), rotation);
        XMVECTOR up = XMVectorSet(0, 1, 0, 0);
        XMVECTOR at = eye + forward;

        XMMATRIX view = XMMatrixLookAtLH(eye, at, up);
        XMMATRIX proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(45.f), (float)m_width / m_height, 0.1f, 5000.f); // 順便把遠裁切面加大到 5000

        // 3. 寫入 Constant Buffer
        SceneConstants cb = {};
        XMStoreFloat4x4(&cb.mvp, XMMatrixTranspose(model * view * proj));

        // 讓光源方向跟著攝影機位置變動 (看起來會有 Headlight 礦工燈的效果)
        XMStoreFloat3(&cb.lightDir, XMVector3Normalize(forward));

		// Normal Matrix 是 Model 的逆轉置矩陣，提供給 Shader 用來正確變換法線向量
        XMStoreFloat4x4(&cb.normalMatrix,
            XMMatrixTranspose(XMMatrixInverse(nullptr, model)));

        cb.baseColor = { 0.8f, 0.6f, 0.4f, 1.0f };
        memcpy(m_cbufferData, &cb, sizeof(cb));

        // 設定 Viewport & Scissor
        D3D12_VIEWPORT vp = { 0,0,(float)m_width,(float)m_height,0,1 };
        D3D12_RECT     sc = { 0,0,m_width,m_height };
        m_cmdList->RSSetViewports(1, &vp);
        m_cmdList->RSSetScissorRects(1, &sc);
        m_cmdList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        // 繪製
        m_cmdList->SetPipelineState(m_pso.Get());
        m_cmdList->SetGraphicsRootSignature(m_rootSig.Get());

        m_cmdList->SetGraphicsRootConstantBufferView(0, m_cbuffer->GetGPUVirtualAddress());
        m_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_cmdList->IASetVertexBuffers(0, 1, &m_mesh->vbView);
        m_cmdList->IASetIndexBuffer(&m_mesh->ibView);

        // 綁定包含所有貼圖的 SRV Heap
        ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
        m_cmdList->SetDescriptorHeaps(1, heaps);

        // 迴圈遍歷所有子網格，分批 Draw Call！
        for (const auto& sub : m_mesh->subMeshes) {

            // 防呆：確保材質索引合法
            int matIdx = sub.materialIndex;
            if (matIdx < 0 || matIdx >= m_mesh->texturePaths.size()) matIdx = 0;

            // 1. 計算這張貼圖在 Descriptor Heap 中的位移量
            CD3DX12_GPU_DESCRIPTOR_HANDLE srvGpuHandle(
                m_srvHeap->GetGPUDescriptorHandleForHeapStart(),
                matIdx,
                m_srvDescriptorSize);

            // 2. 告訴 Shader：「接下來要畫的東西，請用這個位置的貼圖」 (Root Parameter Index 1 對應 t0)
            m_cmdList->SetGraphicsRootDescriptorTable(1, srvGpuHandle);

            // 3. 繪製這個子網格
            m_cmdList->DrawIndexedInstanced(sub.indexCount, 1, sub.indexOffset, 0, 0);
        }
    }

    // Barrier: RenderTarget → Present
    barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        m_renderTargets[m_frameIndex].Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PRESENT);
    m_cmdList->ResourceBarrier(1, &barrier);
    m_cmdList->Close();

    // 指令錄製完畢，立刻解鎖！放開 UI 執行緒！
    m_renderMutex.unlock();

    // 與 GPU 溝通、等待的超耗時動作，絕對不可以拿著鎖做
    ID3D12CommandList* lists[] = { m_cmdList.Get() };
    m_cmdQueue->ExecuteCommandLists(1, lists);
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
    srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

    // 2. 設定 Root Parameters (0 是 MVP 矩陣，1 是貼圖陣列)
    CD3DX12_ROOT_PARAMETER1 rootParameters[2];
    rootParameters[0].InitAsConstantBufferView(0); // 對應 b0
    rootParameters[1].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_PIXEL); // 對應 t0

    // 3. 建立一個預設的靜態採樣器 (s0)
    CD3DX12_STATIC_SAMPLER_DESC sampler(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR);
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
    psoDesc.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
    psoDesc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);

    psoDesc.DepthStencilState.DepthEnable = TRUE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;

    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc = { 1, 0 };

    m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_pso));

    // Constant Buffer（256 byte aligned）
    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    auto bufDesc = CD3DX12_RESOURCE_DESC::Buffer(256);
    m_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE,
        &bufDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&m_cbuffer));
    m_cbuffer->Map(0, nullptr, reinterpret_cast<void**>(&m_cbufferData));
}

void Renderer::UploadMeshToGpu(std::shared_ptr<Mesh> mesh) {
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

    // 根據材質數量重建 SRV Heap (最少給 1 個避免崩潰)
    UINT numMaterials = (std::max)(1, (int)mesh->texturePaths.size());
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.NumDescriptors = numMaterials;
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    CHECK(m_device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&m_srvHeap)));

    m_textures.clear();
    m_textures.resize(numMaterials);
    std::vector<ComPtr<ID3D12Resource>> uploadBuffers(numMaterials); // 暫存的搬運車

    // 準備錄製指令
    m_cmdAllocators[0]->Reset();
    m_cmdList->Reset(m_cmdAllocators[0].Get(), nullptr);

    CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle(m_srvHeap->GetCPUDescriptorHandleForHeapStart());

    // 迴圈處理每一張貼圖
    for (size_t i = 0; i < mesh->texturePaths.size(); i++) {
        int texW = 1, texH = 1, texChannels = 4;
        stbi_uc* pixels = nullptr;
        uint32_t defaultWhite = 0xFFFFFFFF; // 預設白圖 (如果模型沒貼圖)

        if (!mesh->texturePaths[i].empty()) {
            pixels = stbi_load(mesh->texturePaths[i].c_str(), &texW, &texH, &texChannels, 4);
        }

        if (!pixels) {
            pixels = (stbi_uc*)&defaultWhite; // 防呆：給它 1x1 像素的白圖
        }

        // 建立 GPU Texture2D
        auto texDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, texW, texH);
        auto defaultHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &texDesc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_textures[i]));

        // 建立 Upload Buffer
        UINT64 uploadSize;
        m_device->GetCopyableFootprints(&texDesc, 0, 1, 0, nullptr, nullptr, nullptr, &uploadSize);
        auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        auto bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadSize);
        m_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadBuffers[i]));

        // 複製像素資料
        D3D12_SUBRESOURCE_DATA texData = {};
        texData.pData = pixels;
        texData.RowPitch = texW * 4;
        texData.SlicePitch = texData.RowPitch * texH;
        UpdateSubresources(m_cmdList.Get(), m_textures[i].Get(), uploadBuffers[i].Get(), 0, 0, 1, &texData);

        // 轉為 Shader 資源
        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_textures[i].Get(),
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_cmdList->ResourceBarrier(1, &barrier);

        // 建立 SRV 到 Heap 的對應位置
        D3D12_SHADER_RESOURCE_VIEW_DESC srvViewDesc = {};
        srvViewDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvViewDesc.Format = texDesc.Format;
        srvViewDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvViewDesc.Texture2D.MipLevels = 1;
        m_device->CreateShaderResourceView(m_textures[i].Get(), &srvViewDesc, srvHandle);

        // 指標往下移動到下一個空位
        srvHandle.Offset(1, m_srvDescriptorSize);

        if (pixels != (stbi_uc*)&defaultWhite) {
            stbi_image_free(pixels); // 釋放 CPU 記憶體
        }
    }

    // 送出所有搬運指令
    m_cmdList->Close();
    ID3D12CommandList* cmds[] = { m_cmdList.Get() };
    m_cmdQueue->ExecuteCommandLists(1, cmds);
    WaitForGpu(); // 等待所有貼圖上傳完畢 (離開函式後 uploadBuffers 會自動銷毀)
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