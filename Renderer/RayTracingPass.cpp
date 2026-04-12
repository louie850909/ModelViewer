#include "pch.h"
#include "RayTracingPass.h"

void RayTracingPass::CreateRootSignature(ID3D12Device5* device) {
    CD3DX12_DESCRIPTOR_RANGE1 uavRange;
    uavRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE);

    // 材質 SRV 陣列 (t0, space2)
    CD3DX12_DESCRIPTOR_RANGE1 srvRange;
    srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, (UINT)-1, 0, 2, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, 0);

    // 專屬 EnvMap 的 Descriptor Table 範圍 (t1, space0)
    CD3DX12_DESCRIPTOR_RANGE1 envRange;
    envRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 1, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE);

    CD3DX12_ROOT_PARAMETER1 rootParams[6];
    rootParams[0].InitAsDescriptorTable(1, &uavRange);                 // u0, u1: 輸出貼圖
    rootParams[1].InitAsShaderResourceView(0);                         // t0: TLAS (Buffer 可用 Root SRV)
    rootParams[2].InitAsConstantBufferView(0);                         // b0: Camera CB
    rootParams[3].InitAsConstantBufferView(1);                         // b1: Light CB
    rootParams[4].InitAsDescriptorTable(1, &srvRange);                 // t0, space2: 材質 SRV 陣列
    rootParams[5].InitAsDescriptorTable(1, &envRange);                 // t1, space0: EnvMap (紋理必須用 Table)

    CD3DX12_STATIC_SAMPLER_DESC sampler(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR);
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC globalRootSigDesc;
    globalRootSigDesc.Init_1_1(6, rootParams, 1, &sampler);

    ComPtr<ID3DBlob> blob, error;
    HRESULT hr = D3DX12SerializeVersionedRootSignature(&globalRootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1_1, &blob, &error);
    if (FAILED(hr)) throw std::runtime_error("Serialize Root Signature Failed!");

    device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_globalRootSig));
}

void RayTracingPass::CreateLocalRootSignature(ID3D12Device5* device) {
    CD3DX12_ROOT_PARAMETER1 localParams[3];
    localParams[0].InitAsShaderResourceView(0, 1); // t0, space1: Index Buffer
    localParams[1].InitAsShaderResourceView(1, 1); // t1, space1: Vertex Buffer
    localParams[2].InitAsConstants(8, 0, 1, D3D12_SHADER_VISIBILITY_ALL); // b0, space1: textureIndex(1) + transmissionFactor(1) + ior(1) + baseColorFactor(4) = 7 DWORDs → 對齊到 8

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC localRootSigDesc;
    localRootSigDesc.Init_1_1(3, localParams, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE);

    ComPtr<ID3DBlob> blob, error;
    D3DX12SerializeVersionedRootSignature(&localRootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1_1, &blob, &error);
    device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_localRootSig));
}

void RayTracingPass::CreatePipelineState(ID3D12Device5* device) {
    // 為了相容性，這裡使用原生 Array 方式建立 Subobjects
    D3D12_STATE_SUBOBJECT subobjects[7];
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
    // (diffuse 12 + specular 12 + throughput 12 + depth 4 + seed 4 + bool 4)
    shaderConfig.MaxPayloadSizeInBytes = 64;
    shaderConfig.MaxAttributeSizeInBytes = 8;
    subobjects[index++] = { D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shaderConfig };

    // 4. Global Root Signature
    D3D12_GLOBAL_ROOT_SIGNATURE globalRootSig = { m_globalRootSig.Get() };
    subobjects[index++] = { D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &globalRootSig };

    // 5. Pipeline Config (最大遞迴深度)
    D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig = {};
    pipelineConfig.MaxTraceRecursionDepth = 4; // Primary + Bounce + Shadow + Transmission
    subobjects[index++] = { D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipelineConfig };

    // Local Root Signature
    D3D12_LOCAL_ROOT_SIGNATURE localRootSig = { m_localRootSig.Get() };
    subobjects[index++] = { D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, &localRootSig };

    // 將 Local Root Signature 關聯到 HitGroup
    D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION association = {};
    association.NumExports = 1;
    LPCWSTR exportName = L"HitGroup";
    association.pExports = &exportName;
    association.pSubobjectToAssociate = &subobjects[index - 1];
    subobjects[index++] = { D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &association };

    D3D12_STATE_OBJECT_DESC stateObjectDesc = {};
    stateObjectDesc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
    stateObjectDesc.NumSubobjects = index;
    stateObjectDesc.pSubobjects = subobjects;

    HRESULT hr = device->CreateStateObject(&stateObjectDesc, IID_PPV_ARGS(&m_dxrStateObject));
    if (FAILED(hr)) {
        throw std::runtime_error("DXR Pipeline 建立失敗！請檢查 HLSL 語法或 Root Signature 匹配。");
    }
}

// ==========================================
// 動態 BuildSBT
// ==========================================
void RayTracingPass::BuildSBT(ID3D12Device5* device, RenderPassContext& ctx) {
    if (m_instanceCount == 0 || !m_dxrStateObject) return;

    // 每個 HitGroup Record: 32(ID) + 8(IndexBuf VA) + 8(VertexBuf VA) + 16(對齊) + 16(material consts) + padding = 96
    UINT hitGroupStride = 96;

    // ★ 讓 Miss Table 大小加倍，容納兩個 Miss Shader (64 + 128)
    UINT sbtSize = 64 + 128 + (m_instanceCount * hitGroupStride);
    sbtSize = (sbtSize + 255) & ~255; // 256 byte 對齊

    if (!m_sbtBuffer || m_sbtBuffer->GetDesc().Width < sbtSize) {
        auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        auto bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(sbtSize);
        device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_sbtBuffer));
    }

    ComPtr<ID3D12StateObjectProperties> stateObjectProps;
    m_dxrStateObject.As(&stateObjectProps);

    uint8_t* pData;
    m_sbtBuffer->Map(0, nullptr, (void**)&pData);

    memcpy(pData, stateObjectProps->GetShaderIdentifier(L"RayGen"), 32);
    memcpy(pData + 64, stateObjectProps->GetShaderIdentifier(L"Miss"), 32);
    memcpy(pData + 128, stateObjectProps->GetShaderIdentifier(L"ShadowMiss"), 32);

    // HitGroup 起始偏移量往後推
    uint8_t* hitGroupData = pData + 192;

    // 起點為 5 (索引 0,1: UAV, 索引 2,3,4: EnvMap & CDFs)
    UINT destHeapIndex = 5;
    UINT srvDescSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    CD3DX12_CPU_DESCRIPTOR_HANDLE destHandle(m_descriptorHeap->GetCPUDescriptorHandleForHeapStart(), 5, srvDescSize);

    for (auto& inst : ctx.scene->GetMeshes()) {
        auto& mesh = inst.mesh;
        if (!mesh || mesh->blasBuffers.empty()) continue;

        // 將貼圖 SRV 直接建立到 Global Heap 中
        std::vector<UINT> matToGlobalIdx;
        UINT numMats = (UINT)mesh->texturePaths.size();
        if (numMats == 0) numMats = 1; // 至少會有一個預設材質

        for (UINT m = 0; m < numMats; ++m) {
            matToGlobalIdx.push_back(destHeapIndex - 5); // ★ 減 5 對齊 HLSL 的陣列索引 0

			// 每個材質包含 BaseColor, MetallicRoughness, Normal 三種貼圖
            for (int t = 0; t < 3; ++t) {
                int texIdx = m * 3 + t;
                if (texIdx < (int)inst.textures.size() && inst.textures[texIdx]) {
                    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
                    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    sv.Format = inst.textures[texIdx]->GetDesc().Format;
                    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                    sv.Texture2D.MipLevels = inst.textures[texIdx]->GetDesc().MipLevels;
                    device->CreateShaderResourceView(inst.textures[texIdx].Get(), &sv, destHandle);
                }
                else {
                    // 若材質缺失，填入空 SRV 避免 Crash
                    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
                    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                    sv.Texture2D.MipLevels = 1;
                    device->CreateShaderResourceView(nullptr, &sv, destHandle);
                }
                destHandle.Offset(1, srvDescSize);
                destHeapIndex++;
            }
        }

        // 將貼圖索引寫入 HitGroup
        for (size_t n = 0; n < mesh->nodes.size(); ++n) {
            const auto& node = mesh->nodes[n];
            if (node.subMeshIndices.empty()) continue;

            for (int subIdx : node.subMeshIndices) {
                const auto& sub = mesh->subMeshes[subIdx];

                memcpy(hitGroupData, stateObjectProps->GetShaderIdentifier(L"HitGroup"), 32);

                D3D12_GPU_VIRTUAL_ADDRESS* localArgs = (D3D12_GPU_VIRTUAL_ADDRESS*)(hitGroupData + 32);
                // 對齊 HLSL 內的 PrimitiveIndex() 偏移量
                localArgs[0] = mesh->indexBuffer->GetGPUVirtualAddress() + sub.indexOffset * sizeof(uint32_t);
                localArgs[1] = mesh->vertexBuffer->GetGPUVirtualAddress();

                // 綁定 Material Constant
                int matIdx = (sub.materialIndex >= 0 && sub.materialIndex < matToGlobalIdx.size()) ? sub.materialIndex : 0;
                MaterialConstants* mc = reinterpret_cast<MaterialConstants*>(hitGroupData + 32 + 16);
                mc->textureIndex = matToGlobalIdx.empty() ? 0xFFFFFFFF : matToGlobalIdx[matIdx];
                mc->transmissionFactor = sub.transmissionFactor;
                mc->ior = sub.ior;
                mc->baseColorFactor[0] = sub.baseColorFactor[0];
                mc->baseColorFactor[1] = sub.baseColorFactor[1];
                mc->baseColorFactor[2] = sub.baseColorFactor[2];
                mc->baseColorFactor[3] = sub.baseColorFactor[3];
                mc->_pad = 0;

                hitGroupData += hitGroupStride;
            }
        }
    }

    m_sbtBuffer->Unmap(0, nullptr);
    m_sbtHitGroupOffset = 192;// HitGroup 偏移量紀錄
    m_sbtHitGroupStride = hitGroupStride;
}

void RayTracingPass::Init(ID3D12Device* device) {
    ComPtr<ID3D12Device5> device5;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device5)))) return;

    auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    auto bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * m_maxInstances);
    device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_instanceDescBuffer));

    // 建立供 UAV 使用的 Descriptor Heap
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.NumDescriptors = 2048;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_descriptorHeap));

    // 建立相機常數緩衝區 (256 bytes 對齊)
    auto cbDesc = CD3DX12_RESOURCE_DESC::Buffer(256);
    device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &cbDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_cameraCB));
    m_cameraCB->Map(0, nullptr, (void**)&m_mappedCameraCB);

    CreateRootSignature(device5.Get());
    CreateLocalRootSignature(device5.Get());
    CreatePipelineState(device5.Get());
}

void RayTracingPass::EnsureOutputTexture(ID3D12Device* device, int width, int height) {
if (m_outputWidth == width && m_outputHeight == height && m_diffuseOutput != nullptr) return;

    m_outputWidth = width;
    m_outputHeight = height;

    auto defaultHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    auto uavDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT, width, height, 1, 1);
    uavDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    m_diffuseOutput.Reset();
    m_specularOutput.Reset();

    device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &uavDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_diffuseOutput));
    device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &uavDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_specularOutput));

    // 更新 Descriptor Heap 中的 2 個 UAV
    UINT srvUavSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    CD3DX12_CPU_DESCRIPTOR_HANDLE cpuHandle(m_descriptorHeap->GetCPUDescriptorHandleForHeapStart());

    D3D12_UNORDERED_ACCESS_VIEW_DESC viewDesc = {};
    viewDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    
    device->CreateUnorderedAccessView(m_diffuseOutput.Get(), nullptr, &viewDesc, cpuHandle);
    cpuHandle.Offset(1, srvUavSize);
    device->CreateUnorderedAccessView(m_specularOutput.Get(), nullptr, &viewDesc, cpuHandle);
}

void RayTracingPass::BuildTLAS(ID3D12GraphicsCommandList4* cmdList4, RenderPassContext& ctx) {
    auto device5 = ctx.gfx->GetDevice5();
    if (!device5) return;

    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instances;

    using namespace DirectX;
    for (auto& inst : ctx.scene->GetMeshes()) {
        auto& mesh = inst.mesh;
        if (!mesh || mesh->blasBuffers.empty()) continue; // 防呆檢查

        // 計算節點 Global Transform
        std::vector<XMMATRIX> globalTransforms(mesh->nodes.size());
        for (size_t i = 0; i < mesh->nodes.size(); ++i) {
            const auto& node = mesh->nodes[i];
            XMMATRIX local = XMMatrixScaling(node.s[0], node.s[1], node.s[2]) * XMMatrixRotationQuaternion(XMVectorSet(node.r[0], node.r[1], node.r[2], node.r[3])) * XMMatrixTranslation(node.t[0], node.t[1], node.t[2]);
            globalTransforms[i] = (node.parentIndex >= 0) ? local * globalTransforms[node.parentIndex] : local;
        }

        for (size_t n = 0; n < mesh->nodes.size(); ++n) {
            const auto& node = mesh->nodes[n];
            if (node.subMeshIndices.empty()) continue;

            XMMATRIX modelMat = XMMatrixTranspose(globalTransforms[n]);
            XMFLOAT4X4 fMat;
            XMStoreFloat4x4(&fMat, modelMat);

            // 將 Node 所屬的 SubMesh 個別註冊為 TLAS Instance
            for (int subIdx : node.subMeshIndices) {
                if (subIdx >= mesh->blasBuffers.size() || !mesh->blasBuffers[subIdx]) continue;

                D3D12_RAYTRACING_INSTANCE_DESC instanceDesc = {};
                memcpy(instanceDesc.Transform, &fMat, sizeof(float) * 12);

                instanceDesc.InstanceID = (UINT)instances.size();
                instanceDesc.InstanceMask = 0xFF;
                // 利用全域數量確保 HitGroup 嚴格對齊
                instanceDesc.InstanceContributionToHitGroupIndex = (UINT)instances.size();
                instanceDesc.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
                instanceDesc.AccelerationStructure = mesh->blasBuffers[subIdx]->GetGPUVirtualAddress();

                instances.push_back(instanceDesc);
            }
        }
    }

    m_instanceCount = (UINT)instances.size();
    if (m_instanceCount == 0) return;

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
    // ==========================================
    // 透過版本號決定是否重建 TLAS 與 SBT
    // ==========================================
    bool rebuildSBT = (ctx.scene->GetStructureRevision() != m_lastStructureRevision);
    bool rebuildTLAS = rebuildSBT || (ctx.scene->GetTransformRevision() != m_lastTransformRevision);

    if (rebuildTLAS) {
        BuildTLAS(cmdList4.Get(), ctx);
        m_lastTransformRevision = ctx.scene->GetTransformRevision();
    }

    if (m_instanceCount == 0) return; // 場景為空則跳出

    if (rebuildSBT) {
        BuildSBT(ctx.gfx->GetDevice5(), ctx);
        m_lastStructureRevision = ctx.scene->GetStructureRevision();
    }

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
    m_mappedCameraCB->frameCount = ctx.frameCount;

    UINT srvDescSize = ctx.gfx->GetDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto device = ctx.gfx->GetDevice();

    // 只有更換 HDRI 時才建立 Descriptor
    if (m_envMapDirty) {
        CD3DX12_CPU_DESCRIPTOR_HANDLE handleTex(m_descriptorHeap->GetCPUDescriptorHandleForHeapStart(), 2, srvDescSize);
        CD3DX12_CPU_DESCRIPTOR_HANDLE handleMarginal(handleTex, 1, srvDescSize);
        CD3DX12_CPU_DESCRIPTOR_HANDLE handleCond(handleMarginal, 1, srvDescSize);

        if (m_envMap) {
            // 1. EnvMap Texture (t1)
            D3D12_SHADER_RESOURCE_VIEW_DESC texDesc = {};
            texDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            texDesc.Format = m_envMap->texture->GetDesc().Format;
            texDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            texDesc.Texture2D.MipLevels = m_envMap->texture->GetDesc().MipLevels;
            device->CreateShaderResourceView(m_envMap->texture.Get(), &texDesc, handleTex);

            // 2. Marginal CDF Buffer (t2)
            D3D12_SHADER_RESOURCE_VIEW_DESC bufDesc = {};
            bufDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            bufDesc.Format = DXGI_FORMAT_R32_FLOAT; // 以 float 陣列讀取
            bufDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            bufDesc.Buffer.FirstElement = 0;
            bufDesc.Buffer.NumElements = m_envMap->height;
            device->CreateShaderResourceView(m_envMap->marginalCDF.Get(), &bufDesc, handleMarginal);

            // 3. Conditional CDF Buffer (t3)
            bufDesc.Buffer.NumElements = m_envMap->width * m_envMap->height;
            device->CreateShaderResourceView(m_envMap->conditionalCDF.Get(), &bufDesc, handleCond);
        }
        else {
            // 防呆 Null Descriptor
            D3D12_SHADER_RESOURCE_VIEW_DESC nullTex = {};
            nullTex.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            nullTex.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            nullTex.Texture2D.MipLevels = 1;
            device->CreateShaderResourceView(nullptr, &nullTex, handleTex);

            D3D12_SHADER_RESOURCE_VIEW_DESC nullBuf = {};
            nullBuf.Format = DXGI_FORMAT_R32_FLOAT;
            nullBuf.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            nullBuf.Buffer.NumElements = 1;
            device->CreateShaderResourceView(nullptr, &nullBuf, handleMarginal);
            device->CreateShaderResourceView(nullptr, &nullBuf, handleCond);
        }
		m_envMapDirty = false;
    }

    // 綁定 Root 參數
    cmdList4->SetComputeRootDescriptorTable(0, m_descriptorHeap->GetGPUDescriptorHandleForHeapStart()); // UAV (Index 0,1)
    cmdList4->SetComputeRootShaderResourceView(1, m_tlasBuffer->GetGPUVirtualAddress());              // TLAS
    cmdList4->SetComputeRootConstantBufferView(2, m_cameraCB->GetGPUVirtualAddress());                // Camera
    cmdList4->SetComputeRootConstantBufferView(3, ctx.lightCB->GetGPUVirtualAddress());               // Light

    // 綁定材質表 (指向 Index 5)
    CD3DX12_GPU_DESCRIPTOR_HANDLE matTable(m_descriptorHeap->GetGPUDescriptorHandleForHeapStart(), 5, srvDescSize);
    cmdList4->SetComputeRootDescriptorTable(4, matTable);

    // 綁定環境光表 (指向 Index 2，由於 Range 設為 3，它會自動抓取 2,3,4)
    CD3DX12_GPU_DESCRIPTOR_HANDLE envTable(m_descriptorHeap->GetGPUDescriptorHandleForHeapStart(), 2, srvDescSize);
    cmdList4->SetComputeRootDescriptorTable(5, envTable);

    // 設定 SBT 區塊位置與大小
    D3D12_DISPATCH_RAYS_DESC rayDesc = {};
    rayDesc.RayGenerationShaderRecord.StartAddress = m_sbtBuffer->GetGPUVirtualAddress() + 0;
    rayDesc.RayGenerationShaderRecord.SizeInBytes = 64;

    rayDesc.MissShaderTable.StartAddress = m_sbtBuffer->GetGPUVirtualAddress() + 64;
    rayDesc.MissShaderTable.SizeInBytes = 128; // 包含兩個 Miss Shader
    rayDesc.MissShaderTable.StrideInBytes = 64;

    // 套用動態的 HitGroup 設定
    rayDesc.HitGroupTable.StartAddress = m_sbtBuffer->GetGPUVirtualAddress() + m_sbtHitGroupOffset;
    rayDesc.HitGroupTable.SizeInBytes = m_instanceCount * m_sbtHitGroupStride;
    rayDesc.HitGroupTable.StrideInBytes = m_sbtHitGroupStride;

    rayDesc.Width = ctx.gfx->GetWidth();
    rayDesc.Height = ctx.gfx->GetHeight();
    rayDesc.Depth = 1;

    // 發射！
    cmdList4->DispatchRays(&rayDesc);
    // // 將原始輸出交給 Context，讓下一個 Pass (Denoiser) 接手
    ctx.rawDiffuseGI = m_diffuseOutput.Get();
    ctx.rawSpecularGI = m_specularOutput.Get();
}