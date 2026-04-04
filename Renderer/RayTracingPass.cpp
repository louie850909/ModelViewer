#include "pch.h"
#include "RayTracingPass.h"

void RayTracingPass::Init(ID3D12Device* device) {
    ComPtr<ID3D12Device5> device5;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device5)))) return;

    // 建立 Instance Descriptors 的 Upload Buffer
    auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    auto bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * m_maxInstances);
    device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_instanceDescBuffer));

    // (後續階段將在此建立 DXR State Object 與 Root Signature)
}

void RayTracingPass::EnsureOutputTexture(ID3D12Device* device, int width, int height) {
    if (m_outputWidth == width && m_outputHeight == height && m_raytracingOutput != nullptr) return;

    m_outputWidth = width;
    m_outputHeight = height;

    auto defaultHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    auto uavDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, width, height, 1, 1);
    uavDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &uavDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_raytracingOutput));
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

    // 1. 每幀更新 TLAS
    BuildTLAS(cmdList4.Get(), ctx);

    // 2. (測試用) 先將 DXR 輸出貼圖清除為亮紫色，證明已進入 DXR Pass
    float clearColor[4] = { 0.8f, 0.2f, 0.8f, 1.0f };
    // [註: 此處為了測試階段簡化，正式光追時會由 DispatchRays 寫入]

    // 3. 將 UAV 複製到 BackBuffer 展示
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