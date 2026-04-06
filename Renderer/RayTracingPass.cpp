#include "pch.h"
#include "RayTracingPass.h"

void RayTracingPass::CreateRootSignature(ID3D12Device5* device) {
    CD3DX12_DESCRIPTOR_RANGE1 uavRange;
    uavRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);

    CD3DX12_ROOT_PARAMETER1 rootParams[3];
    rootParams[0].InitAsDescriptorTable(1, &uavRange);                 // u0: 輸出貼圖
    rootParams[1].InitAsShaderResourceView(0);                         // t0: TLAS
    rootParams[2].InitAsConstantBufferView(0);                         // b0: Camera CB

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC globalRootSigDesc;
    globalRootSigDesc.Init_1_1(3, rootParams);

    ComPtr<ID3DBlob> blob, error;
    D3DX12SerializeVersionedRootSignature(&globalRootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1_1, &blob, &error);
    device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_globalRootSig));
}

void RayTracingPass::CreatePipelineState(ID3D12Device5* device) {
    // 為了相容性，這裡使用原生 Array 方式建立 Subobjects
    D3D12_STATE_SUBOBJECT subobjects[5];
    UINT index = 0;

    // 1. DXIL Library (載入編譯好的 CSO)
    ComPtr<ID3DBlob> dxilBlob;
    D3DReadFileToBlob(GetShaderPath(L"Raytracing.cso").c_str(), &dxilBlob);
    D3D12_DXIL_LIBRARY_DESC dxilLibDesc = {};
    D3D12_SHADER_BYTECODE dxilBytecode = { dxilBlob->GetBufferPointer(), dxilBlob->GetBufferSize() };
    dxilLibDesc.DXILLibrary = dxilBytecode;
    subobjects[index++] = { D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &dxilLibDesc };

    // 2. Hit Group
    D3D12_HIT_GROUP_DESC hitGroupDesc = {};
    hitGroupDesc.HitGroupExport = L"HitGroup";
    hitGroupDesc.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
    hitGroupDesc.ClosestHitShaderImport = L"ClosestHit";
    subobjects[index++] = { D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hitGroupDesc };

    // 3. Shader Config (Payload 大小)
    D3D12_RAYTRACING_SHADER_CONFIG shaderConfig = {};
    shaderConfig.MaxPayloadSizeInBytes = 16; // float4 color
    shaderConfig.MaxAttributeSizeInBytes = 8; // float2 barycentrics
    subobjects[index++] = { D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shaderConfig };

    // 4. Global Root Signature
    D3D12_GLOBAL_ROOT_SIGNATURE globalRootSig = { m_globalRootSig.Get() };
    subobjects[index++] = { D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &globalRootSig };

    // 5. Pipeline Config (最大遞迴深度)
    D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig = {};
    pipelineConfig.MaxTraceRecursionDepth = 1;
    subobjects[index++] = { D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipelineConfig };

    D3D12_STATE_OBJECT_DESC stateObjectDesc = {};
    stateObjectDesc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
    stateObjectDesc.NumSubobjects = index;
    stateObjectDesc.pSubobjects = subobjects;

    device->CreateStateObject(&stateObjectDesc, IID_PPV_ARGS(&m_dxrStateObject));
}

void RayTracingPass::CreateSBT(ID3D12Device5* device) {
    ComPtr<ID3D12StateObjectProperties> stateObjectProps;
    m_dxrStateObject.As(&stateObjectProps);

    // 配置 256 bytes (64 bytes 給 RayGen, 64 給 Miss, 64 給 HitGroup，完全符合對齊要求)
    auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    auto bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(256);
    device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_sbtBuffer));

    uint8_t* pData;
    m_sbtBuffer->Map(0, nullptr, (void**)&pData);

    // 寫入 Shader Identifiers (每個長度為 32 bytes，寫入起點以 64 bytes 分隔)
    memcpy(pData + 0, stateObjectProps->GetShaderIdentifier(L"RayGen"), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
    memcpy(pData + 64, stateObjectProps->GetShaderIdentifier(L"Miss"), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
    memcpy(pData + 128, stateObjectProps->GetShaderIdentifier(L"HitGroup"), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);

    m_sbtBuffer->Unmap(0, nullptr);
}

void RayTracingPass::Init(ID3D12Device* device) {
    ComPtr<ID3D12Device5> device5;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device5)))) return;

    auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    auto bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * m_maxInstances);
    device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_instanceDescBuffer));

    // 建立供 UAV 使用的 Descriptor Heap
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.NumDescriptors = 1;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_descriptorHeap));

    // 建立相機常數緩衝區 (256 bytes 對齊)
    auto cbDesc = CD3DX12_RESOURCE_DESC::Buffer(256);
    device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &cbDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_cameraCB));
    m_cameraCB->Map(0, nullptr, (void**)&m_mappedCameraCB);

    CreateRootSignature(device5.Get());
    CreatePipelineState(device5.Get());
    CreateSBT(device5.Get());
}

void RayTracingPass::EnsureOutputTexture(ID3D12Device* device, int width, int height) {
    if (m_outputWidth == width && m_outputHeight == height && m_raytracingOutput != nullptr) return;

    m_outputWidth = width;
    m_outputHeight = height;

    auto defaultHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    auto uavDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, width, height, 1, 1);
    uavDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &uavDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_raytracingOutput));

    // 每次重建輸出貼圖時，一併更新 Descriptor Heap 中的 UAV
    D3D12_UNORDERED_ACCESS_VIEW_DESC viewDesc = {};
    viewDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(m_raytracingOutput.Get(), nullptr, &viewDesc, m_descriptorHeap->GetCPUDescriptorHandleForHeapStart());
}

void RayTracingPass::BuildTLAS(ID3D12GraphicsCommandList4* cmdList4, RenderPassContext& ctx) {
    auto device5 = ctx.gfx->GetDevice5();
    if (!device5) return;

    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instances;

    using namespace DirectX;
    for (auto& inst : ctx.scene->GetMeshes()) {
        auto& mesh = inst.mesh;
        if (!mesh || !mesh->blasBuffer) continue;

        // 計算節點 Global Transform
        std::vector<XMMATRIX> globalTransforms(mesh->nodes.size());
        for (size_t i = 0; i < mesh->nodes.size(); ++i) {
            const auto& node = mesh->nodes[i];
            XMMATRIX local = XMMatrixScaling(node.s[0], node.s[1], node.s[2]) * XMMatrixRotationQuaternion(XMVectorSet(node.r[0], node.r[1], node.r[2], node.r[3])) * XMMatrixTranslation(node.t[0], node.t[1], node.t[2]);
            globalTransforms[i] = (node.parentIndex >= 0) ? local * globalTransforms[node.parentIndex] : local;
        }

        // 為每個 SubMesh 建立 Instance (此處假設整個 Mesh 共用一個 BLAS 以簡化實作)
        for (size_t n = 0; n < mesh->nodes.size(); ++n) {
            const auto& node = mesh->nodes[n];
            if (node.subMeshIndices.empty()) continue;

            XMMATRIX modelMat = XMMatrixTranspose(globalTransforms[n]); // DXR 使用 Row-Major 3x4

            D3D12_RAYTRACING_INSTANCE_DESC instanceDesc = {};
            // 將 4x4 矩陣前三列填入 Transform
            XMFLOAT4X4 fMat;
            XMStoreFloat4x4(&fMat, modelMat);
            memcpy(instanceDesc.Transform, &fMat, sizeof(float) * 12);

            instanceDesc.InstanceID = (UINT)n;
            instanceDesc.InstanceMask = 0xFF;
            instanceDesc.InstanceContributionToHitGroupIndex = 0; // 後續 SBT 會用到
            instanceDesc.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
            instanceDesc.AccelerationStructure = mesh->blasBuffer->GetGPUVirtualAddress();

            instances.push_back(instanceDesc);
        }
    }

    if (instances.empty()) return;

    // 將資料寫入 Upload Buffer
    void* mappedData;
    m_instanceDescBuffer->Map(0, nullptr, &mappedData);
    memcpy(mappedData, instances.data(), instances.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
    m_instanceDescBuffer->Unmap(0, nullptr);

    // 準備 TLAS 建置輸入
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    inputs.NumDescs = (UINT)instances.size();
    inputs.pGeometryDescs = nullptr;
    inputs.InstanceDescs = m_instanceDescBuffer->GetGPUVirtualAddress();
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
    device5->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);

    // 重新配置 TLAS 緩衝區 (若大小改變)
    if (!m_tlasBuffer || m_tlasBuffer->GetDesc().Width < info.ResultDataMaxSizeInBytes) {
        auto defaultHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        auto tlasDesc = CD3DX12_RESOURCE_DESC::Buffer(info.ResultDataMaxSizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        auto scratchDesc = CD3DX12_RESOURCE_DESC::Buffer(info.ScratchDataSizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

        device5->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &tlasDesc, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nullptr, IID_PPV_ARGS(&m_tlasBuffer));
        device5->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &scratchDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_tlasScratch));
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {};
    buildDesc.Inputs = inputs;
    buildDesc.DestAccelerationStructureData = m_tlasBuffer->GetGPUVirtualAddress();
    buildDesc.ScratchAccelerationStructureData = m_tlasScratch->GetGPUVirtualAddress();

    cmdList4->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);

    auto uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(m_tlasBuffer.Get());
    cmdList4->ResourceBarrier(1, &uavBarrier);
}

void RayTracingPass::Execute(ID3D12GraphicsCommandList* cmdList, RenderPassContext& ctx) {
    ComPtr<ID3D12GraphicsCommandList4> cmdList4;
    if (FAILED(cmdList->QueryInterface(IID_PPV_ARGS(&cmdList4)))) return;

    EnsureOutputTexture(ctx.gfx->GetDevice(), ctx.gfx->GetWidth(), ctx.gfx->GetHeight());
    BuildTLAS(cmdList4.Get(), ctx);

    // ==========================================
    // 光線追蹤派發 (Dispatch Rays)
    // ==========================================

    // 綁定管線與 Descriptor Heap
    cmdList4->SetPipelineState1(m_dxrStateObject.Get());
    ID3D12DescriptorHeap* heaps[] = { m_descriptorHeap.Get() };
    cmdList4->SetDescriptorHeaps(1, heaps);
    cmdList4->SetComputeRootSignature(m_globalRootSig.Get());

    // 更新與綁定相機參數
    using namespace DirectX;
    XMMATRIX viewProj = ctx.view * ctx.proj;
    XMVECTOR det;
    XMStoreFloat4x4(&m_mappedCameraCB->viewProjInv, XMMatrixTranspose(XMMatrixInverse(&det, viewProj)));
    m_mappedCameraCB->cameraPos = ctx.scene->GetCameraPos();

    // 綁定 Root 參數
    cmdList4->SetComputeRootDescriptorTable(0, m_descriptorHeap->GetGPUDescriptorHandleForHeapStart()); // UAV
    cmdList4->SetComputeRootShaderResourceView(1, m_tlasBuffer->GetGPUVirtualAddress());              // TLAS
    cmdList4->SetComputeRootConstantBufferView(2, m_cameraCB->GetGPUVirtualAddress());                // Camera

    // 設定 SBT 區塊位置與大小
    D3D12_DISPATCH_RAYS_DESC rayDesc = {};
    rayDesc.RayGenerationShaderRecord.StartAddress = m_sbtBuffer->GetGPUVirtualAddress() + 0;
    rayDesc.RayGenerationShaderRecord.SizeInBytes = 64;

    rayDesc.MissShaderTable.StartAddress = m_sbtBuffer->GetGPUVirtualAddress() + 64;
    rayDesc.MissShaderTable.SizeInBytes = 64;
    rayDesc.MissShaderTable.StrideInBytes = 64;

    rayDesc.HitGroupTable.StartAddress = m_sbtBuffer->GetGPUVirtualAddress() + 128;
    rayDesc.HitGroupTable.SizeInBytes = 64;
    rayDesc.HitGroupTable.StrideInBytes = 64;

    rayDesc.Width = ctx.gfx->GetWidth();
    rayDesc.Height = ctx.gfx->GetHeight();
    rayDesc.Depth = 1;

    // 發射！
    cmdList4->DispatchRays(&rayDesc);

    // ==========================================
    // 準備複製到 BackBuffer
    // ==========================================
    auto backBuffer = ctx.gfx->GetCurrentBackBuffer();
    D3D12_RESOURCE_BARRIER preCopy[2] = {
        CD3DX12_RESOURCE_BARRIER::Transition(backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST),
        CD3DX12_RESOURCE_BARRIER::Transition(m_raytracingOutput.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE)
    };
    cmdList->ResourceBarrier(2, preCopy);

    cmdList->CopyResource(backBuffer, m_raytracingOutput.Get());

    D3D12_RESOURCE_BARRIER postCopy[2] = {
        CD3DX12_RESOURCE_BARRIER::Transition(backBuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET),
        CD3DX12_RESOURCE_BARRIER::Transition(m_raytracingOutput.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    };
    cmdList->ResourceBarrier(2, postCopy);
}