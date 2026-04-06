RaytracingAccelerationStructure Scene : register(t0, space0);
RWTexture2D<float4> RenderTarget : register(u0, space0);

cbuffer CameraParams : register(b0, space0)
{
    float4x4 viewProjInv;
    float3 cameraPos;
    float pad;
};

struct Vertex
{
    float3 position;
    float3 normal;
    float2 uv;
};

// 宣告區域根簽章 (Local Root Signature) 的變數 (配置於 space1 避免與全域衝突)
ByteAddressBuffer IndexBuffer : register(t0, space1);
StructuredBuffer<Vertex> VertexBuffer : register(t1, space1);

struct Payload
{
    float4 color;
};

[shader("raygeneration")]
void RayGen()
{
    uint2 launchIndex = DispatchRaysIndex().xy;
    uint2 launchDim = DispatchRaysDimensions().xy;

    // 將螢幕座標轉換到 NDC 空間 (-1 到 1)
    float2 d = (((float2) launchIndex + 0.5f) / (float2) launchDim) * 2.0f - 1.0f;
    d.y = -d.y;

    // 透過反矩陣將 NDC 解投影回世界空間，取得射線方向
    float4 target = mul(float4(d.x, d.y, 1.0f, 1.0f), viewProjInv);
    float3 rayDir = normalize((target.xyz / target.w) - cameraPos);

    RayDesc ray;
    ray.Origin = cameraPos;
    ray.Direction = rayDir;
    ray.TMin = 0.001f;
    ray.TMax = 10000.0f;

    Payload payload;
    payload.color = float4(0, 0, 0, 0);

    // 發射光線
    TraceRay(Scene, RAY_FLAG_NONE, 0xFF, 0, 1, 0, ray, payload);

    RenderTarget[launchIndex] = payload.color;
}

[shader("miss")]
void Miss(inout Payload payload)
{
    payload.color = float4(0.2f, 0.4f, 0.8f, 1.0f);
}

[shader("closesthit")]
void ClosestHit(inout Payload payload, in BuiltInTriangleIntersectionAttributes attr)
{
    uint primitiveIndex = PrimitiveIndex();
    
    // 透過 Index Buffer 找出這三個頂點的索引 (DXGI_FORMAT_R32_UINT，每個 Triangle 佔 12 Bytes)
    uint i0 = IndexBuffer.Load(primitiveIndex * 12 + 0);
    uint i1 = IndexBuffer.Load(primitiveIndex * 12 + 4);
    uint i2 = IndexBuffer.Load(primitiveIndex * 12 + 8);

    // 取得 Vertex Normals
    float3 n0 = VertexBuffer[i0].normal;
    float3 n1 = VertexBuffer[i1].normal;
    float3 n2 = VertexBuffer[i2].normal;

    // 利用重心座標進行法線插值
    float3 barycentrics = float3(1.0f - attr.barycentrics.x - attr.barycentrics.y, attr.barycentrics.x, attr.barycentrics.y);
    float3 localNormal = n0 * barycentrics.x + n1 * barycentrics.y + n2 * barycentrics.z;
    localNormal = normalize(localNormal);

    // 將法線轉至世界空間 (利用 WorldToObject 矩陣的轉置來正確處理非等比縮放)
    float3x4 mInv = WorldToObject3x4();
    float3 worldNormal = normalize(mul(localNormal, (float3x3) mInv));

    // 簡易的 Diffuse 光照模型
    float3 lightDir = normalize(float3(1.0f, 1.0f, -1.0f));
    float ndotl = max(0.0f, dot(worldNormal, lightDir));
    
    float3 diffuse = float3(0.9f, 0.9f, 0.9f) * ndotl;
    float3 ambient = float3(0.15f, 0.15f, 0.2f);

    payload.color = float4(diffuse + ambient, 1.0f);
}