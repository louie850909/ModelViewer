#include "pch.h"
#include "RayTracingPass.h"

void RayTracingPass::CreateRootSignature(ID3D12Device5* device) {
    CD3DX12_DESCRIPTOR_RANGE1 uavRange;
    uavRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE);

    // 材質 SRV 配列 (t0, space2)
    CD3DX12_DESCRIPTOR_RANGE1 srvRange;
    srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, (UINT)-1, 0, 2, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, 0);

    // EnvMap 専用の Descriptor Table 範囲 (t1, space0)
    CD3DX12_DESCRIPTOR_RANGE1 envRange;
    envRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 1, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE);

    CD3DX12_ROOT_PARAMETER1 rootParams[6];
    rootParams[0].InitAsDescriptorTable(1, &uavRange);                 // u0, u1: 出力テクスチャ
    rootParams[1].InitAsShaderResourceView(0);                         // t0: TLAS (Buffer は Root SRV 使用可能)
    rootParams[2].InitAsConstantBufferView(0);                         // b0: Camera CB
    rootParams[3].InitAsConstantBufferView(1);                         // b1: Light CB
    rootParams[4].InitAsDescriptorTable(1, &srvRange);                 // t0, space2: 材質 SRV 配列
    rootParams[5].InitAsDescriptorTable(1, &envRange);                 // t1, space0: EnvMap (テクスチャは Table を使用する必要がある)

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
    localParams[2].InitAsConstants(10, 0, 1, D3D12_SHADER_VISIBILITY_ALL); // b0, space1: textureIndex(1) + transmissionFactor(1) + ior(1) + baseColorFactor(4) + roughnessFactor(1) + metallicFactor(1) = 9 DWORDs → 10 にアライメント

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC localRootSigDesc;
    localRootSigDesc.Init_1_1(3, localParams, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE);

    ComPtr<ID3DBlob> blob, error;
    D3DX12SerializeVersionedRootSignature(&localRootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1_1, &blob, &error);
    device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_localRootSig));
}

void RayTracingPass::CreatePipelineState(ID3D12Device5* device) {
    // 互換性のため、ネイティブ Array 方式で Subobjects を作成
    D3D12_STATE_SUBOBJECT subobjects[12];
    UINT index = 0;

    // 1. DXIL Library (コンパイル済み CSO を読み込む)
    ComPtr<ID3DBlob> dxilBlob;
    D3DReadFileToBlob(GetShaderPath(L"Raytracing.cso").c_str(), &dxilBlob);
    D3D12_DXIL_LIBRARY_DESC dxilLibDesc = {};
    D3D12_SHADER_BYTECODE dxilBytecode = { dxilBlob->GetBufferPointer(), dxilBlob->GetBufferSize() };
    dxilLibDesc.DXILLibrary = dxilBytecode;
    subobjects[index++] = { D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &dxilLibDesc };

    // --- 2. 4 種類の異なる HitGroup を作成 ---
    // (A) 主光線 - 不透明 (最速、AnyHit なし)
    D3D12_HIT_GROUP_DESC hgPriOpaque = { L"HitGroup_Primary_Opaque", D3D12_HIT_GROUP_TYPE_TRIANGLES, nullptr, L"ClosestHit", nullptr };
    subobjects[index++] = { D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hgPriOpaque };

    // (B) 主光線 - 透明マスク (AnyHit を含む)
    D3D12_HIT_GROUP_DESC hgPriAlpha = { L"HitGroup_Primary_Alpha", D3D12_HIT_GROUP_TYPE_TRIANGLES, L"AnyHit", L"ClosestHit", nullptr };
    subobjects[index++] = { D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hgPriAlpha };

    // (C) シャドウ光線 - 不透明 (空の HitGroup、DXR のデフォルト遮蔽に依存)
    D3D12_HIT_GROUP_DESC hgShadowOpaque = { L"HitGroup_Shadow_Opaque", D3D12_HIT_GROUP_TYPE_TRIANGLES, nullptr, nullptr, nullptr };
    subobjects[index++] = { D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hgShadowOpaque };

    // (D) シャドウ光線 - 透明マスク (ShadowAnyHit のみ含む)
    D3D12_HIT_GROUP_DESC hgShadowAlpha = { L"HitGroup_Shadow_Alpha", D3D12_HIT_GROUP_TYPE_TRIANGLES, L"ShadowAnyHit", nullptr, nullptr };
    subobjects[index++] = { D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hgShadowAlpha };

    // 3. Shader Config (Payload サイズ)
    D3D12_RAYTRACING_SHADER_CONFIG shaderConfig = {};
    // (diffuse 12 + specular 12 + throughput 12 + depth 4 + seed 4 + bool 4)
    shaderConfig.MaxPayloadSizeInBytes = 64;
    shaderConfig.MaxAttributeSizeInBytes = 8;
    subobjects[index++] = { D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shaderConfig };

    // 4. Global Root Signature
    D3D12_GLOBAL_ROOT_SIGNATURE globalRootSig = { m_globalRootSig.Get() };
    subobjects[index++] = { D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &globalRootSig };

    // 5. Pipeline Config (最大再帰深度)
    D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig = {};
    pipelineConfig.MaxTraceRecursionDepth = 4; // Primary + Bounce + Shadow + Transmission
    subobjects[index++] = { D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipelineConfig };

    // Local Root Signature
    D3D12_LOCAL_ROOT_SIGNATURE localRootSig = { m_localRootSig.Get() };
    subobjects[index++] = { D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, &localRootSig };

    // Local Root Signature を HitGroup に関連付ける
    D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION association = {};
    LPCWSTR hitGroupExports[] = { L"HitGroup_Primary_Opaque", L"HitGroup_Primary_Alpha", L"HitGroup_Shadow_Opaque", L"HitGroup_Shadow_Alpha" };
    association.NumExports = 4;
    association.pExports = hitGroupExports;
    association.pSubobjectToAssociate = &subobjects[index - 1]; // Local Root Sig を指す
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

    // 各 HitGroup Record: 32(ID) + 8(IndexBuf VA) + 8(VertexBuf VA) + 16(アライメント) + 16(material consts) + padding = 96
    UINT hitGroupStride = 96;

    // ★ Miss Table のサイズを 2 倍にして 2 つの Miss Shader を格納 (64 + 128)
    UINT sbtSize = 64 + 128 + (m_instanceCount * hitGroupStride * 2);
    sbtSize = (sbtSize + 255) & ~255; // 256 byte アライメント

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

    // 起点は 5 (インデックス 0,1: UAV、インデックス 2,3,4: EnvMap & CDFs)
    UINT destHeapIndex = 5;
    UINT srvDescSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    CD3DX12_CPU_DESCRIPTOR_HANDLE destHandle(m_descriptorHeap->GetCPUDescriptorHandleForHeapStart(), 5, srvDescSize);

    for (auto& inst : ctx.scene->GetMeshes()) {
        auto& mesh = inst.mesh;
        if (!mesh || mesh->blasBuffers.empty()) continue;

        // テクスチャ SRV を直接 Global Heap に作成
        std::vector<UINT> matToGlobalIdx;
        UINT numMats = (UINT)mesh->texturePaths.size();
        if (numMats == 0) numMats = 1; // 少なくともデフォルト材質が 1 つある

        for (UINT m = 0; m < numMats; ++m) {
            matToGlobalIdx.push_back(destHeapIndex - 5); // ★ 5 を引いて HLSL の配列インデックス 0 に合わせる

			// 各材質は BaseColor、MetallicRoughness、Normal の 3 種類のテクスチャを含む
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
                    // 材質が欠損している場合、クラッシュを防ぐため空 SRV を挿入
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

        // テクスチャインデックスを HitGroup に書き込む
        for (size_t n = 0; n < mesh->nodes.size(); ++n) {
            const auto& node = mesh->nodes[n];
            if (node.subMeshIndices.empty()) continue;

            for (int subIdx : node.subMeshIndices) {
                const auto& sub = mesh->subMeshes[subIdx];
                // この SubMesh が透明チャンネルを含むかどうかを取得
                bool needsAnyHit = sub.isAlphaTested;

                // ==========================================
                // 第一のレコードを書き込む: Primary Ray HitGroup
                // ==========================================
                LPCWSTR priHitGroup = needsAnyHit ? L"HitGroup_Primary_Alpha" : L"HitGroup_Primary_Opaque";
                memcpy(hitGroupData, stateObjectProps->GetShaderIdentifier(priHitGroup), 32);

                D3D12_GPU_VIRTUAL_ADDRESS* localArgs = (D3D12_GPU_VIRTUAL_ADDRESS*)(hitGroupData + 32);
                localArgs[0] = mesh->indexBuffer->GetGPUVirtualAddress() + sub.indexOffset * sizeof(uint32_t);
                localArgs[1] = mesh->vertexBuffer->GetGPUVirtualAddress();

                int matIdx = (sub.materialIndex >= 0 && sub.materialIndex < matToGlobalIdx.size()) ? sub.materialIndex : 0;
                MaterialConstants* mc = reinterpret_cast<MaterialConstants*>(hitGroupData + 32 + 16);
                mc->textureIndex = matToGlobalIdx.empty() ? 0xFFFFFFFF : matToGlobalIdx[matIdx];
                mc->transmissionFactor = sub.transmissionFactor;
                mc->ior = sub.ior;
                mc->baseColorFactor[0] = sub.baseColorFactor[0];
                mc->baseColorFactor[1] = sub.baseColorFactor[1];
                mc->baseColorFactor[2] = sub.baseColorFactor[2];
                mc->baseColorFactor[3] = sub.baseColorFactor[3];
                mc->roughnessFactor = sub.roughnessFactor;
                mc->metallicFactor  = sub.metallicFactor;
                mc->_pad = 0;

                hitGroupData += hitGroupStride; // 次のレコードに進む

                // ==========================================
                // 第二のレコードを書き込む: Shadow Ray HitGroup
                // ==========================================
                LPCWSTR shadowHitGroup = needsAnyHit ? L"HitGroup_Shadow_Alpha" : L"HitGroup_Shadow_Opaque";
                memcpy(hitGroupData, stateObjectProps->GetShaderIdentifier(shadowHitGroup), 32);

                // まったく同じ Local Arguments をシャドウ光線用にコピー (ShadowAnyHit もテクスチャを読む必要があるため)
                D3D12_GPU_VIRTUAL_ADDRESS* localArgsShadow = (D3D12_GPU_VIRTUAL_ADDRESS*)(hitGroupData + 32);
                localArgsShadow[0] = localArgs[0];
                localArgsShadow[1] = localArgs[1];
                MaterialConstants* mcShadow = reinterpret_cast<MaterialConstants*>(hitGroupData + 32 + 16);
                memcpy(mcShadow, mc, sizeof(MaterialConstants));

                hitGroupData += hitGroupStride; // 再度前進
            }
        }
    }

    m_sbtBuffer->Unmap(0, nullptr);
    m_sbtHitGroupOffset = 192;// HitGroup オフセット量を記録
    m_sbtHitGroupStride = hitGroupStride;
}

void RayTracingPass::Init(ID3D12Device* device) {
    ComPtr<ID3D12Device5> device5;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device5)))) return;

    auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    auto bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * m_maxInstances);
    device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_instanceDescBuffer));

    // UAV 用の Descriptor Heap を作成
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.NumDescriptors = 2048;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_descriptorHeap));

    // カメラ定数バッファを作成 (256 bytes アライメント)
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

    // Descriptor Heap 内の 2 つの UAV を更新
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
        if (!mesh || mesh->blasBuffers.empty()) continue; // フェイルセーフチェック

        // ノードの Global Transform を計算
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

            // ノードに属する SubMesh を個別に TLAS Instance として登録
            for (int subIdx : node.subMeshIndices) {
                if (subIdx >= mesh->blasBuffers.size() || !mesh->blasBuffers[subIdx]) continue;

                D3D12_RAYTRACING_INSTANCE_DESC instanceDesc = {};
                memcpy(instanceDesc.Transform, &fMat, sizeof(float) * 12);

                instanceDesc.InstanceID = (UINT)instances.size();
                instanceDesc.InstanceMask = 0xFF;
                // グローバル数を使用して HitGroup の厳密なアライメントを確保
				instanceDesc.InstanceContributionToHitGroupIndex = (UINT)instances.size() * 2; // 各 Instance は 1 つの HitGroup に対応し、HitGroup の間隔は 2 (Primary + Shadow)
                instanceDesc.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
                instanceDesc.AccelerationStructure = mesh->blasBuffers[subIdx]->GetGPUVirtualAddress();

                instances.push_back(instanceDesc);
            }
        }
    }

    m_instanceCount = (UINT)instances.size();
    if (m_instanceCount == 0) return;

    // Upload Buffer にデータを書き込む
    void* mappedData;
    m_instanceDescBuffer->Map(0, nullptr, &mappedData);
    memcpy(mappedData, instances.data(), instances.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
    m_instanceDescBuffer->Unmap(0, nullptr);

    // TLAS 構築入力を準備
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    inputs.NumDescs = (UINT)instances.size();
    inputs.pGeometryDescs = nullptr;
    inputs.InstanceDescs = m_instanceDescBuffer->GetGPUVirtualAddress();
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
    device5->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);

    // TLAS バッファを再配置 (サイズが変更された場合)
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
    // バージョン番号で TLAS と SBT の再構築を決定
    // ==========================================
    bool rebuildSBT = (ctx.scene->GetStructureRevision() != m_lastStructureRevision);
    bool rebuildTLAS = rebuildSBT || (ctx.scene->GetTransformRevision() != m_lastTransformRevision);

    if (rebuildTLAS) {
        BuildTLAS(cmdList4.Get(), ctx);
        m_lastTransformRevision = ctx.scene->GetTransformRevision();
    }

    if (m_instanceCount == 0) return; // シーンが空の場合は終了

    if (rebuildSBT) {
        BuildSBT(ctx.gfx->GetDevice5(), ctx);
        m_lastStructureRevision = ctx.scene->GetStructureRevision();
    }

    // ==========================================
    // レイトレーシング ディスパッチ (Dispatch Rays)
    // ==========================================
    // パイプラインと Descriptor Heap をバインド
    cmdList4->SetPipelineState1(m_dxrStateObject.Get());
    ID3D12DescriptorHeap* heaps[] = { m_descriptorHeap.Get() };
    cmdList4->SetDescriptorHeaps(1, heaps);
    cmdList4->SetComputeRootSignature(m_globalRootSig.Get());

    // カメラパラメータを更新してバインド
    using namespace DirectX;
    XMMATRIX viewProj = ctx.view * ctx.proj;
    XMVECTOR det;
    XMStoreFloat4x4(&m_mappedCameraCB->viewProjInv, XMMatrixTranspose(XMMatrixInverse(&det, viewProj)));
    m_mappedCameraCB->cameraPos = ctx.scene->GetCameraPos();
    m_mappedCameraCB->frameCount = ctx.frameCount;
    m_mappedCameraCB->envIntegral = (m_envMap != nullptr) ? m_envMap->envIntegral : 1.0f;
    // ピクセル空間の jitter を NDC オフセットに変換 (透過ピクセルで打ち消すため)
    m_mappedCameraCB->jitterX = ctx.jitterX * 2.0f / (float)ctx.gfx->GetWidth();
    m_mappedCameraCB->jitterY = ctx.jitterY * 2.0f / (float)ctx.gfx->GetHeight();

    UINT srvDescSize = ctx.gfx->GetDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto device = ctx.gfx->GetDevice();

    // HDRI が変更された場合のみ Descriptor を作成
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
            bufDesc.Format = DXGI_FORMAT_R32_FLOAT; // float 配列として読み込む
            bufDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            bufDesc.Buffer.FirstElement = 0;
            bufDesc.Buffer.NumElements = m_envMap->height;
            device->CreateShaderResourceView(m_envMap->marginalCDF.Get(), &bufDesc, handleMarginal);

            // 3. Conditional CDF Buffer (t3)
            bufDesc.Buffer.NumElements = m_envMap->width * m_envMap->height;
            device->CreateShaderResourceView(m_envMap->conditionalCDF.Get(), &bufDesc, handleCond);
        }
        else {
            // フェイルセーフ Null Descriptor
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

    // Root パラメータをバインド
    cmdList4->SetComputeRootDescriptorTable(0, m_descriptorHeap->GetGPUDescriptorHandleForHeapStart()); // UAV (Index 0,1)
    cmdList4->SetComputeRootShaderResourceView(1, m_tlasBuffer->GetGPUVirtualAddress());              // TLAS
    cmdList4->SetComputeRootConstantBufferView(2, m_cameraCB->GetGPUVirtualAddress());                // Camera
    cmdList4->SetComputeRootConstantBufferView(3, ctx.lightCB->GetGPUVirtualAddress());               // Light

    // 材質テーブルをバインド (Index 5 を指す)
    CD3DX12_GPU_DESCRIPTOR_HANDLE matTable(m_descriptorHeap->GetGPUDescriptorHandleForHeapStart(), 5, srvDescSize);
    cmdList4->SetComputeRootDescriptorTable(4, matTable);

    // 環境光テーブルをバインド (Index 2 を指す、Range が 3 に設定されているため 2,3,4 が自動的に取得される)
    CD3DX12_GPU_DESCRIPTOR_HANDLE envTable(m_descriptorHeap->GetGPUDescriptorHandleForHeapStart(), 2, srvDescSize);
    cmdList4->SetComputeRootDescriptorTable(5, envTable);

    // SBT ブロックの位置とサイズを設定
    D3D12_DISPATCH_RAYS_DESC rayDesc = {};
    rayDesc.RayGenerationShaderRecord.StartAddress = m_sbtBuffer->GetGPUVirtualAddress() + 0;
    rayDesc.RayGenerationShaderRecord.SizeInBytes = 64;

    rayDesc.MissShaderTable.StartAddress = m_sbtBuffer->GetGPUVirtualAddress() + 64;
    rayDesc.MissShaderTable.SizeInBytes = 128; // 2 つの Miss Shader を含む
    rayDesc.MissShaderTable.StrideInBytes = 64;

    // 動的な HitGroup 設定を適用
    rayDesc.HitGroupTable.StartAddress = m_sbtBuffer->GetGPUVirtualAddress() + m_sbtHitGroupOffset;
    rayDesc.HitGroupTable.SizeInBytes = m_instanceCount * m_sbtHitGroupStride * 2;
    rayDesc.HitGroupTable.StrideInBytes = m_sbtHitGroupStride;

    rayDesc.Width = ctx.gfx->GetWidth();
    rayDesc.Height = ctx.gfx->GetHeight();
    rayDesc.Depth = 1;

    // 発射！
    cmdList4->DispatchRays(&rayDesc);
    // // 將原始輸出交給 Context，讓下一個 Pass (Denoiser) 接手
    ctx.rawDiffuseGI = m_diffuseOutput.Get();
    ctx.rawSpecularGI = m_specularOutput.Get();
}