RaytracingAccelerationStructure Scene : register(t0, space0);
RWTexture2D<float4> RenderTarget : register(u0, space0);

cbuffer CameraParams : register(b0, space0)
{
    float4x4 viewProjInv;
    float3 cameraPos;
    float pad;
};

struct Light
{
    int type;
    float intensity;
    float coneAngle;
    float _pad1;
    float3 color;
    float _pad2;
    float3 position;
    float _pad3;
    float3 direction;
    float _pad4;
};

cbuffer LightBuffer : register(b1, space0)
{
    int numLights;
    float3 _padLights;
    Light lights[16];
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

// 告知這個 SubMesh 該用哪張貼圖
cbuffer MaterialConstants : register(b0, space1)
{
    uint textureIndex;
};

// ==========================================
// 全域的無綁定貼圖陣列與採樣器
// ==========================================
Texture2D allTextures[] : register(t0, space2);
SamplerState texSampler : register(s0, space0);

struct Payload
{
    float4 color;
};

// 陰影射線專用的 Payload
struct ShadowPayload
{
    bool isHit;
};

[shader("miss")]
void ShadowMiss(inout ShadowPayload payload)
{
    // 沒打中任何東西，代表沒有遮蔽物 (不在陰影中)
    payload.isHit = false;
}

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
    
    // 取得三個頂點的 UV
    float2 uv0 = VertexBuffer[i0].uv;
    float2 uv1 = VertexBuffer[i1].uv;
    float2 uv2 = VertexBuffer[i2].uv;

    // 利用重心座標進行法線插值
    float3 barycentrics = float3(1.0f - attr.barycentrics.x - attr.barycentrics.y, attr.barycentrics.x, attr.barycentrics.y);
    float3 localNormal = n0 * barycentrics.x + n1 * barycentrics.y + n2 * barycentrics.z;
    localNormal = normalize(localNormal);
    
    // 插值計算交點的 UV
    float2 localUV = uv0 * barycentrics.x + uv1 * barycentrics.y + uv2 * barycentrics.z;

    // 將法線轉至世界空間 (利用 WorldToObject 矩陣的轉置來正確處理非等比縮放)
    float3x4 mInv = WorldToObject3x4();
    float3 worldNormal = normalize(mul(localNormal, (float3x3) mInv));
    
    // 計算交點的世界座標
    float3 worldPos = WorldRayOrigin() + WorldRayDirection() * RayTCurrent();

    float3 finalColor = float3(0, 0, 0);
    float3 baseColor = float3(0.9f, 0.9f, 0.9f); // 暫時代替紋理的純白材質
    
    // ==========================================
    // 從全域陣列中採樣貼圖
    // ==========================================
    if (textureIndex != 0xFFFFFFFF)
    {
        // 必須使用 NonUniformResourceIndex 避免 GPU 執行緒在存取不同貼圖時發生死結
        float4 texColor = allTextures[NonUniformResourceIndex(textureIndex)].SampleLevel(texSampler, localUV, 0);
        // 將 Albedo 的 sRGB 色彩轉回線性空間 (Linear Space) 讓光照計算更寫實
        baseColor = pow(texColor.rgb, 2.2f);
    }

    // 尋訪所有光源
    for (int i = 0; i < numLights; i++)
    {
        Light L = lights[i];
        float3 lightDir;
        float attenuation = 1.0f;
        float shadowTMax = 10000.0f;

        if (L.type == 0)
        { // Directional Light
            lightDir = normalize(-L.direction);
        }
        else
        { // Point Light
            float3 d = L.position - worldPos;
            float dist = length(d);
            lightDir = d / dist;
            attenuation = 1.0f / (1.0f + 0.1f * dist + 0.01f * dist * dist);
            shadowTMax = dist; // 陰影射線只需測量到光源的距離
        }

        float ndotl = max(0.0f, dot(worldNormal, lightDir));

        if (ndotl > 0.0f)
        {
            // ★ 發射陰影射線
            RayDesc shadowRay;
            shadowRay.Origin = worldPos;
            shadowRay.Direction = lightDir;
            shadowRay.TMin = 0.01f; // 加上極小偏移值，避免自體陰影 (Shadow Acne)
            shadowRay.TMax = shadowTMax;

            ShadowPayload shadowPayload;
            shadowPayload.isHit = true; // 預設當作被遮蔽

            // 發送射線：遇到任意物體即停止，且跳過 ClosestHit 以增進效能。MissShader 索引為 1 (ShadowMiss)
            TraceRay(Scene,
                     RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
                     0xFF, 0, 1, 1, shadowRay, shadowPayload);

            if (!shadowPayload.isHit)
            {
                finalColor += baseColor * L.color * L.intensity * ndotl * attenuation;
            }
        }
    }

    float3 ambient = float3(0.1f, 0.1f, 0.15f) * baseColor;
    payload.color = float4(finalColor + ambient, 1.0f);
}