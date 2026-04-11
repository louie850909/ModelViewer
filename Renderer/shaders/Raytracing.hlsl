#include "PBRCommon.hlsli"

RaytracingAccelerationStructure Scene : register(t0, space0);
RWTexture2D<float4> DiffuseTarget : register(u0, space0);
RWTexture2D<float4> SpecularTarget : register(u1, space0);

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

// 產生在 3x3 空間內分佈非常均勻的低差異雜訊，特別適合 SVGF 降噪
float IGN(float2 pixelPos)
{
    float3 magic = float3(0.06711056f, 0.00583715f, 52.9829189f);
    return frac(magic.z * frac(dot(pixelPos, magic.xy)));
}

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
    float3 diffuse; // 收集到的漫反射能量
    uint depth; // 目前彈跳次數
    float3 specular; // 收集到的高光能量
    uint seed; // 亂數種子
    float3 throughput; // 光線衰減權重
    bool isSpecularBounce; // 標記這條光線在第一次彈跳時是否走了高光路徑
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

// --- Miss Shader ---
[shader("miss")]
void Miss(inout Payload payload)
{
    float3 skyColor = float3(0.2f, 0.4f, 0.8f);
    // 根據路徑屬性，將天空顏色存入對應的能量槽
    if (payload.depth == 0)
    {
        payload.diffuse += payload.throughput * skyColor;
    }
    else
    {
        if (payload.isSpecularBounce)
            payload.specular += payload.throughput * skyColor;
        else
            payload.diffuse += payload.throughput * skyColor;
    }
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

    const uint SPP = 2;
    float3 accumDiffuse = float3(0, 0, 0);
    float3 accumSpecular = float3(0, 0, 0);

    // 使用 IGN 結合時間與空間，產生低差異性的亂數基底
    // 每幀加上一個基於黃金比例的偏移量，打亂時間軸的規律，避免雜訊固定在畫面上
    float frameOffset = (float) (frameCount % 256) * 1.61803398f;
    float spatialTemporalNoise = IGN(float2(launchIndex.x, launchIndex.y) + frameOffset);

    [unroll]
    for (uint s = 0; s < SPP; ++s)
    {
        Payload payload;
        payload.diffuse = float3(0, 0, 0);
        payload.specular = float3(0, 0, 0);
        payload.throughput = float3(1, 1, 1);
        payload.depth = 0;
        payload.isSpecularBounce = false;
        
        // 將 IGN 的浮點數轉為 uint，再丟給 pcg_hash 徹底攪拌
        // 這樣既保留了 IGN 的空間均勻性，又具備 pcg_hash 的去關聯性
        payload.seed = pcg_hash(asuint(spatialTemporalNoise) ^ (s * 114514u));

        RayDesc ray;
        ray.Origin = cameraPos;
        ray.Direction = rayDir;
        ray.TMin = 0.001f;
        ray.TMax = 10000.0f;

        TraceRay(Scene, RAY_FLAG_NONE, 0xFF, 0, 1, 0, ray, payload);

        accumDiffuse += payload.diffuse;
        accumSpecular += payload.specular;
    }

    // 分別輸出到兩張貼圖
    DiffuseTarget[launchIndex] = float4(accumDiffuse / (float) SPP, 1.0f);
    SpecularTarget[launchIndex] = float4(accumSpecular / (float) SPP, 1.0f);
}
[shader("closesthit")]
void ClosestHit(inout Payload payload, in BuiltInTriangleIntersectionAttributes attr)
{
    uint primitiveIndex = PrimitiveIndex();
    
    // 透過 Index Buffer 找出這三個頂點的索引
    uint i0 = IndexBuffer.Load(primitiveIndex * 12 + 0);
    uint i1 = IndexBuffer.Load(primitiveIndex * 12 + 4);
    uint i2 = IndexBuffer.Load(primitiveIndex * 12 + 8);
    
    float3 n0 = VertexBuffer[i0].normal;
    float3 n1 = VertexBuffer[i1].normal;
    float3 n2 = VertexBuffer[i2].normal;
    
    float2 uv0 = VertexBuffer[i0].uv;
    float2 uv1 = VertexBuffer[i1].uv;
    float2 uv2 = VertexBuffer[i2].uv;
    
    // 利用重心座標進行插值
    float3 barycentrics = float3(1.0f - attr.barycentrics.x - attr.barycentrics.y, attr.barycentrics.x, attr.barycentrics.y);
    float3 localNormal = normalize(n0 * barycentrics.x + n1 * barycentrics.y + n2 * barycentrics.z);
    float2 localUV = uv0 * barycentrics.x + uv1 * barycentrics.y + uv2 * barycentrics.z;
    
    float3x4 mInv = WorldToObject3x4();
    float3 worldNormal = normalize(mul(localNormal, (float3x3) mInv));
    float3 worldPos = WorldRayOrigin() + WorldRayDirection() * RayTCurrent();
    
    // 攝影機視角方向
    float3 V = normalize(cameraPos - worldPos);

    // ==========================================
    // 1. 採樣 PBR 貼圖 (Albedo, Roughness, Metallic)
    // ==========================================
    float3 baseColor = float3(0.9f, 0.9f, 0.9f);
    float roughness = 0.5f;
    float metallic = 0.0f;

    if (textureIndex != 0xFFFFFFFF)
    {
        // 第一張貼圖：BaseColor
        float4 texColor = allTextures[NonUniformResourceIndex(textureIndex)].SampleLevel(texSampler, localUV, 0);
        baseColor = pow(texColor.rgb, 2.2f); // sRGB 轉線性空間
        
        // 第二張貼圖：MetallicRoughness (緊接在 BaseColor 後面)
        float4 mrColor = allTextures[NonUniformResourceIndex(textureIndex + 1)].SampleLevel(texSampler, localUV, 0);
        // glTF 標準: Green 頻道 = 粗糙度, Blue 頻道 = 金屬度
        roughness = clamp(mrColor.g, 0.05f, 1.0f); // 避免 0 造成 NaN
        metallic = saturate(mrColor.b);
    }

    // 基礎反射率：非金屬預設為 4%，金屬則帶有原本的顏色
    float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), baseColor, metallic);

    // ==========================================
    // 2. 計算直接光照 (Direct Illumination + GGX Specular)
    // ==========================================
    float3 directLighting = float3(0, 0, 0);

    for (int i = 0; i < numLights; i++)
    {
        Light L = lights[i];
        float3 lightDir;
        float attenuation = 1.0f;
        float shadowTMax = 10000.0f;

        if (L.type == 0) // 平行光
        {
            lightDir = normalize(-L.direction);
        }
        else // 點光源
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
            // 陰影判定
            RayDesc shadowRay;
            shadowRay.Origin = worldPos + worldNormal * 0.001f;
            shadowRay.Direction = lightDir;
            shadowRay.TMin = 0.01f;
            shadowRay.TMax = shadowTMax;

            ShadowPayload shadowPayload;
            shadowPayload.isHit = true;
            TraceRay(Scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER, 0xFF, 0, 1, 1, shadowRay, shadowPayload);

            if (!shadowPayload.isHit)
            {
                // --- PBR 核心光照計算 ---
                float3 H = normalize(V + lightDir);
                float NDF = DistributionGGX(worldNormal, H, roughness);
                float G = GeometrySmith(worldNormal, V, lightDir, roughness);
                float3 F = FresnelSchlick(max(dot(H, V), 0.0f), F0);

                // 高光項 (Specular)
                float3 numerator = NDF * G * F;
                float denominator = 4.0f * max(dot(worldNormal, V), 0.0f) * ndotl + 0.0001f;
                float3 specular = numerator / denominator;

                // 漫反射項 (Diffuse)
                float3 kD = (1.0f - F) * (1.0f - metallic);
                float3 diff = kD * baseColor / PI;

                float3 currentDirectDiffuse = diff * L.color * L.intensity * ndotl * attenuation;
                float3 currentDirectSpecular = specular * L.color * L.intensity * ndotl * attenuation;

                // 精確拆分直接光照
                if (payload.depth == 0)
                {
                    payload.diffuse += payload.throughput * currentDirectDiffuse;
                    payload.specular += payload.throughput * currentDirectSpecular;
                }
                else
                {
                    // 如果是後續彈跳，根據第一次彈跳的屬性來歸類能量
                    if (payload.isSpecularBounce)
                    {
                        payload.specular += payload.throughput * (currentDirectDiffuse + currentDirectSpecular);
                    }
                    else
                    {
                        payload.diffuse += payload.throughput * (currentDirectDiffuse + currentDirectSpecular);
                    }
                }
            }
        }
    }

    // ==========================================
    // 3. 計算間接光照與材質反射 (Indirect Bounce)
    // ==========================================
    if (payload.depth < 1)
    {
        bool isFirstBounce = (payload.depth == 0);
        payload.depth++;

        // 計算菲涅爾效應，並以此做為發生「鏡面反射」的機率
        float3 F = FresnelSchlick(max(dot(worldNormal, V), 0.0f), F0);
        float specProbability = clamp(lerp(max(F.r, max(F.g, F.b)), 1.0f, metallic), 0.05f, 0.95f);

        float randomVal = rand(payload.seed);
        float3 bounceDir;

        if (randomVal < specProbability)
        {
            // 走鏡面反射路徑 (GGX VNDF 重要性採樣)
            if (isFirstBounce)
                payload.isSpecularBounce = true;

            float alpha = max(roughness * roughness, 0.001f);
            float2 u = float2(rand(payload.seed), rand(payload.seed));

            // 1. 建立切線空間矩陣
            float3x3 tbn = GetTBN(worldNormal);
            
            // 2. 將視角方向轉換到切線空間 (World -> Local)
            float3 V_local = normalize(mul(tbn, V));
            
            // 3. 使用 VNDF 採樣微表面法線 (Half-vector)
            float3 H_local = SampleGGXVNDF(V_local, alpha, alpha, u.x, u.y);
            
            // 4. 將微表面法線轉回世界空間 (Local -> World)
            float3 H = normalize(mul(H_local, tbn));
            
            // 5. 計算反射方向
            bounceDir = reflect(-V, H);

            // 6. 計算 shadowing-masking 權重 (G2 / G1) 以確保能量守恆
            // 因為 VNDF 採樣的機率密度函數 (PDF) 剛好與 BRDF 中的 D 和 G1 抵銷，
            // 所以我們只需乘上 Fresnel 與幾何遮蔽比率 (G2/G1) 即可。
            float G1 = GeometrySchlickGGX(max(dot(worldNormal, V), 0.0f), roughness);
            float G2 = GeometrySmith(worldNormal, V, bounceDir, roughness);
            float G_weight = (G1 > 0.0001f) ? (G2 / G1) : 0.0f;

            payload.throughput *= (F * G_weight) / specProbability;
        }
        else
        {
            // 走漫反射路徑
            if (isFirstBounce)
                payload.isSpecularBounce = false; // 紀錄為漫反射路徑

            bounceDir = getCosineWeightedSample(worldNormal, payload.seed);
            float3 kD = (1.0f - F) * (1.0f - metallic);
            payload.throughput *= (kD * baseColor) / (1.0f - specProbability);
        }

        // 發射下一彈跳的光線
        if (dot(bounceDir, worldNormal) > 0.0f)
        {
            RayDesc bounceRay;
            bounceRay.Origin = worldPos + worldNormal * 0.001f;
            bounceRay.Direction = bounceDir;
            bounceRay.TMin = 0.01f;
            bounceRay.TMax = 10000.0f;

            TraceRay(Scene, RAY_FLAG_NONE, 0xFF, 0, 1, 0, bounceRay, payload);
        }
    }
}