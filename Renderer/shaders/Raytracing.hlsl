RaytracingAccelerationStructure Scene : register(t0, space0);
RWTexture2D<float4> RenderTarget : register(u0, space0);

cbuffer CameraParams : register(b0, space0)
{
    float4x4 viewProjInv;
    float3 cameraPos;
    uint frameCount;
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

ByteAddressBuffer IndexBuffer : register(t0, space1);
StructuredBuffer<Vertex> VertexBuffer : register(t1, space1);

cbuffer MaterialConstants : register(b0, space1)
{
    uint textureIndex;
};

Texture2D allTextures[] : register(t0, space2);
SamplerState texSampler : register(s0, space0);

// ==========================================
// 亂數與取樣工具
// ==========================================
uint pcg_hash(uint seed)
{
    uint state = seed * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float rand(inout uint seed)
{
    seed = pcg_hash(seed);
    return (float) seed / 4294967296.0f;
}

float3 getCosineWeightedSample(float3 n, inout uint seed)
{
    float u1 = rand(seed);
    float u2 = rand(seed);

    float r = sqrt(u1);
    float theta = 2.0f * 3.14159265f * u2;

    float x = r * cos(theta);
    float y = r * sin(theta);
    float z = sqrt(max(0.0f, 1.0f - u1));

    // 建立切線空間 (Tangent Space)
    float3 up = abs(n.z) < 0.999f ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 t = normalize(cross(up, n));
    float3 b = cross(n, t);

    return normalize(t * x + b * y + n * z);
}

// ==========================================
// Payloads
// ==========================================
struct Payload
{
    float3 radiance; // 累計發光量
    float3 throughput; // 光線衰減權重
    uint depth; // 目前彈跳次數
    uint seed; // 亂數種子
};

struct ShadowPayload
{
    bool isHit;
};

// ==========================================
// Shaders
// ==========================================
[shader("miss")]
void ShadowMiss(inout ShadowPayload payload)
{
    payload.isHit = false;
}

[shader("miss")]
void Miss(inout Payload payload)
{
    // 打到天空：將天空顏色乘上衰減權重加進結果
    float3 skyColor = float3(0.2f, 0.4f, 0.8f);
    payload.radiance += payload.throughput * skyColor;
}

[shader("raygeneration")]
void RayGen()
{
    uint2 launchIndex = DispatchRaysIndex().xy;
    uint2 launchDim = DispatchRaysDimensions().xy;

    float2 d = (((float2) launchIndex + 0.5f) / (float2) launchDim) * 2.0f - 1.0f;
    d.y = -d.y;

    float4 target = mul(float4(d.x, d.y, 1.0f, 1.0f), viewProjInv);
    float3 rayDir = normalize((target.xyz / target.w) - cameraPos);

    RayDesc ray;
    ray.Origin = cameraPos;
    ray.Direction = rayDir;
    ray.TMin = 0.001f;
    ray.TMax = 10000.0f;

    Payload payload;
    payload.radiance = float3(0, 0, 0);
    payload.throughput = float3(1, 1, 1);
    payload.depth = 0;
    // 使用像素座標與影格數作為亂數種子，避免靜態雜訊
    payload.seed = pcg_hash(launchIndex.y * launchDim.x + launchIndex.x + frameCount * 719393u);

    TraceRay(Scene, RAY_FLAG_NONE, 0xFF, 0, 1, 0, ray, payload);

    RenderTarget[launchIndex] = float4(payload.radiance, 1.0f);
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

    float3 baseColor = float3(0.9f, 0.9f, 0.9f);
    
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

    // --- 計算直接光照 ---
    float3 directLighting = float3(0, 0, 0);
    for (int i = 0; i < numLights; i++)
    {
        Light L = lights[i];
        float3 lightDir;
        float attenuation = 1.0f;
        float shadowTMax = 10000.0f;

        if (L.type == 0)
        {
            lightDir = normalize(-L.direction);
        }
        else
        {
            float3 d = L.position - worldPos;
            float dist = length(d);
            lightDir = d / dist;
            attenuation = 1.0f / (1.0f + 0.1f * dist + 0.01f * dist * dist);
            shadowTMax = dist;
        }

        float ndotl = max(0.0f, dot(worldNormal, lightDir));

        if (ndotl > 0.0f)
        {
            RayDesc shadowRay;
            shadowRay.Origin = worldPos;
            shadowRay.Direction = lightDir;
            shadowRay.TMin = 0.01f;
            shadowRay.TMax = shadowTMax;

            ShadowPayload shadowPayload;
            shadowPayload.isHit = true;

            TraceRay(Scene,
                     RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
                     0xFF, 0, 1, 1, shadowRay, shadowPayload);

            if (!shadowPayload.isHit)
            {
                directLighting += baseColor * L.color * L.intensity * ndotl * attenuation;
            }
        }
    }

    // 將直接光照乘上衰減權重，累加到輻射率 (Radiance) 中
    payload.radiance += payload.throughput * directLighting;

    // --- GI 間接光反射 (限制最多 1 次 Bounce) ---
    if (payload.depth < 1)
    {
        payload.depth++;
        
        // 使用漫反射的重要性採樣 (Cosine-Weighted)
        // 此處 PDF = cos(theta)/PI, 而 Lambertian BRDF = BaseColor/PI
        // 相除後恰好為 BaseColor，無需再乘上 ndotl 或除以 PI
        payload.throughput *= baseColor;

        // 取得半球面上隨機彈跳的方向
        float3 bounceDir = getCosineWeightedSample(worldNormal, payload.seed);

        RayDesc bounceRay;
        bounceRay.Origin = worldPos;
        bounceRay.Direction = bounceDir;
        bounceRay.TMin = 0.01f;
        bounceRay.TMax = 10000.0f;

        // 遞迴發射間接光線，下一層的顏色會自動加到 payload 中
        TraceRay(Scene, RAY_FLAG_NONE, 0xFF, 0, 1, 0, bounceRay, payload);
    }
}