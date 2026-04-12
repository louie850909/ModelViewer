#include "PBRCommon.hlsli"

RaytracingAccelerationStructure Scene : register(t0, space0);
RWTexture2D<float4> DiffuseTarget : register(u0, space0);
RWTexture2D<float4> SpecularTarget : register(u1, space0);
Texture2D<float4> EnvMap : register(t1, space0);
Buffer<float> EnvMarginalCDF : register(t2, space0); // 邊緣機率分佈 (1D)
Buffer<float> EnvConditionalCDF : register(t3, space0); // 條件機率分佈 (2D)

cbuffer CameraParams : register(b0, space0)
{
    float4x4 viewProjInv;
    float3 cameraPos;
    uint frameCount;
    float envIntegral; // 環境光總能量 (供 PDF 計算)
    float3 _padCamera; // 補齊 16 bytes 對齊
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
    float transmissionFactor;
    float ior;
    float baseColorFactor_r;
    float baseColorFactor_g;
    float baseColorFactor_b;
    float baseColorFactor_a;
    uint _matPad;
};

Texture2D allTextures[] : register(t0, space2);
SamplerState texSampler : register(s0, space0);

static const float INV_PI = 0.318309886f;
static const float INV_TWO_PI = 0.159154943f;

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

// 將 3D 方向向量轉換為 2D 球面 UV 座標
float2 GetSphericalUV(float3 v)
{
    // 假設 Y 軸朝上 (Y-Up)
    // atan2 取得水平經度 [-PI, PI] -> u
    // asin 取得垂直緯度 [-PI/2, PI/2] -> v
    float2 uv = float2(atan2(v.z, v.x), asin(v.y));
    
    // 映射到 [-0.5, 0.5]
    uv *= float2(INV_TWO_PI, INV_PI);
    
    // 映射到 [0, 1]
    uv += 0.5f;
    
    // DirectX 的 V 座標起點在上方，故反轉 V
    uv.y = 1.0f - uv.y;
    
    return uv;
}

// ==========================================
// HDRI 輔助搜尋與採樣 (NEE 核心)
// ==========================================
uint FindIntervalMarginal(float u, uint EnvHeight)
{
    uint left = 0;
    uint right = EnvHeight - 1;
    while (left < right)
    {
        uint mid = (left + right) >> 1;
        if (EnvMarginalCDF[mid] < u)
            left = mid + 1;
        else
            right = mid;
    }
    return left;
}

uint FindIntervalConditional(uint y, float u, uint EnvWidth)
{
    uint left = 0;
    uint right = EnvWidth - 1;
    uint offset = y * EnvWidth;
    while (left < right)
    {
        uint mid = (left + right) >> 1;
        if (EnvConditionalCDF[offset + mid] < u)
            left = mid + 1;
        else
            right = mid;
    }
    return left;
}

struct EnvSampleResult
{
    float3 direction;
    float3 color;
    float pdf;
};

EnvSampleResult SampleEnv(float2 u, float envInt)
{
    uint EnvWidth, EnvHeight;
    EnvMap.GetDimensions(EnvWidth, EnvHeight);

    // 透過二分搜尋取得對應的高亮度像素位置
    uint y = FindIntervalMarginal(u.y, EnvHeight);
    uint x = FindIntervalConditional(y, u.x, EnvWidth);
    float2 uv = float2((x + 0.5f) / EnvWidth, (y + 0.5f) / EnvHeight);

    // 將 UV 轉回 3D 空間方向 (反解 GetSphericalUV)
    float v_real = 1.0f - uv.y;
    float phi = (uv.x - 0.5f) * 2.0f * PI;
    float theta = (v_real - 0.5f) * PI;

    float3 dir;
    dir.y = sin(theta);
    float cosTheta = cos(theta);
    dir.z = sin(phi) * cosTheta;
    dir.x = cos(phi) * cosTheta;

    float3 color = EnvMap.SampleLevel(texSampler, uv, 0).rgb;
    float luma = dot(color, float3(0.2126f, 0.7152f, 0.0722f));

    EnvSampleResult res;
    res.direction = normalize(dir);
    res.color = color;
    // 將亮度除以總能量，得到該方向的機率密度 (PDF)
    res.pdf = max(luma / max(envInt, 0.0001f), 0.0001f);
    return res;
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
    float lastRayPdf; // 上一段光線的 BRDF 機率密度
    float lastRoughness; // 上一段反射表面的粗糙度
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
    float3 rayDir = WorldRayDirection();
    float2 envUV = GetSphericalUV(rayDir);
    float3 skyColor = EnvMap.SampleLevel(texSampler, envUV, 0).rgb;

    // 計算我們如果用 NEE 去採樣這片天空，機率密度是多少
    float luma = dot(skyColor, float3(0.2126f, 0.7152f, 0.0722f));
    float pdf_env = max(luma / max(envIntegral, 0.0001f), 0.0001f);

    float misWeight = 1.0f;
    
    // 如果這是一條間接反彈射線，執行 MIS
    if (payload.depth > 0)
    {
        if (payload.lastRoughness < 0.05f)
        {
            // 非常光滑的表面 (近乎鏡面)，NEE 很難主動採樣到，這時完全依賴 BRDF 盲射
            misWeight = 1.0f;
        }
        else
        {
            // Power Heuristic 公式: BRDF² / (BRDF² + ENV²)
            float pdf_brdf = payload.lastRayPdf;
            misWeight = (pdf_brdf * pdf_brdf) / (pdf_brdf * pdf_brdf + pdf_env * pdf_env);
        }
    }

    skyColor *= misWeight;

    if (payload.depth == 0)
        payload.diffuse += payload.throughput * skyColor;
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

    const uint SPP = 1;
    float3 accumDiffuse = float3(0, 0, 0);
    float3 accumSpecular = float3(0, 0, 0);

    uint linearIndex = launchIndex.y * launchDim.x + launchIndex.x;
    
    [unroll]
    for (uint s = 0; s < SPP; ++s)
    {
        Payload payload;
        payload.diffuse = float3(0, 0, 0);
        payload.specular = float3(0, 0, 0);
        payload.throughput = float3(1, 1, 1);
        payload.depth = 0;
        payload.isSpecularBounce = false;
        payload.lastRayPdf = 0.0f;
        payload.lastRoughness = 0.0f;
        
        payload.seed = pcg_hash(linearIndex ^ (frameCount * 114514u) ^ (s * 1973u));

        RayDesc ray;
        ray.Origin = cameraPos;
        ray.Direction = rayDir;
        ray.TMin = 0.001f;
        ray.TMax = 10000.0f;

        TraceRay(Scene, RAY_FLAG_NONE, 0xFF, 0, 1, 0, ray, payload);

        // 螢火蟲抑制 (Firefly Clamping)
        // 限制單次光線反彈帶回來的最大能量，避免 HDRI 極亮點摧毀時域累積的歷史
        const float MAX_RADIANCE = 20.0f; // 可視您的 HDRI 亮度上下微調此數值

        float lumaD = dot(payload.diffuse, float3(0.2126f, 0.7152f, 0.0722f));
        if (lumaD > MAX_RADIANCE) 
            payload.diffuse *= (MAX_RADIANCE / lumaD);

        float lumaS = dot(payload.specular, float3(0.2126f, 0.7152f, 0.0722f));
        if (lumaS > MAX_RADIANCE) 
            payload.specular *= (MAX_RADIANCE / lumaS);

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

    // 先把 baseColorFactor 轉到線性空間
    float3 baseColorFactorLinear = float3(
        pow(baseColorFactor_r, 2.2f),
        pow(baseColorFactor_g, 2.2f),
        pow(baseColorFactor_b, 2.2f)
    );

    if (textureIndex != 0xFFFFFFFF)
    {
        // 有貼圖：貼圖色 * baseColorFactor
        float4 texColor = allTextures[NonUniformResourceIndex(textureIndex)].SampleLevel(texSampler, localUV, 0);
        baseColor = pow(texColor.rgb, 2.2f) * baseColorFactorLinear;

        float4 mrColor = allTextures[NonUniformResourceIndex(textureIndex + 1)].SampleLevel(texSampler, localUV, 0);
        roughness = clamp(mrColor.g, 0.05f, 1.0f);
        metallic = saturate(mrColor.b);
        
        float3 localNormalMap = allTextures[NonUniformResourceIndex(textureIndex + 2)].SampleLevel(texSampler, localUV, 0).xyz;
        localNormalMap = localNormalMap * 2.0f - 1.0f; // 轉換到 [-1, 1]
        
        // 反轉 Y 軸以修正 glTF (OpenGL) 與 DX12 的法線系統差異
        localNormalMap.y = -localNormalMap.y;

        float3 e1 = VertexBuffer[i1].position - VertexBuffer[i0].position;
        float3 e2 = VertexBuffer[i2].position - VertexBuffer[i0].position;
        float2 duv1 = uv1 - uv0;
        float2 duv2 = uv2 - uv0;
        
        float det = duv1.x * duv2.y - duv1.y * duv2.x;
        float3 worldTangent;
        float r = 1.0f / det;
        
        // 【防呆機制】避免 UV 退化導致除以零 (NaN)
        if (abs(det) < 1e-6f)
        {
            // 若沒有有效的 UV，給予一個預設切線避免崩潰
            float3 up = abs(worldNormal.z) < 0.999f ? float3(0, 0, 1) : float3(1, 0, 0);
            worldTangent = normalize(cross(up, worldNormal));
        }
        else
        {
            float3 objTangent = (e1 * duv2.y - e2 * duv1.y) * r;
            float3x4 mO2W = ObjectToWorld3x4();
            worldTangent = normalize(mul((float3x3) mO2W, objTangent));
        }
        
        // 確保正交化
        worldTangent = normalize(worldTangent - dot(worldTangent, worldNormal) * worldNormal);
        // 利用 r 的正負號修正鏡像 UV 的副切線方向
        float handedness = (r < 0.0f) ? -1.0f : 1.0f;
        float3 worldBitangent = cross(worldNormal, worldTangent) * handedness;
        
        // 覆蓋掉原本平滑的 worldNormal，讓後續的折射與反射都有凹凸細節！
        float3x3 tbn = float3x3(worldTangent, worldBitangent, worldNormal);
        worldNormal = normalize(mul(localNormalMap, tbn));
    }
    else
    {
        // 無貼圖（如 PawnTopWhite）：直接用 baseColorFactor
        baseColor = baseColorFactorLinear;
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
    
    
    float3 F_bounce = FresnelSchlick(max(dot(worldNormal, V), 0.0f), F0);
    float specProbability = clamp(lerp(max(F_bounce.r, max(F_bounce.g, F_bounce.b)), 1.0f, metallic), 0.05f, 0.95f);
    // ==========================================
    // 2.5 環境光直接採樣 (Next Event Estimation + MIS)
    // ==========================================
    // 對於有一定粗糙度的表面，主動尋找 HDRI 中的高亮點
    if (roughness >= 0.05f)
    {
        float2 uEnv = float2(rand(payload.seed), rand(payload.seed));
        EnvSampleResult envSample = SampleEnv(uEnv, envIntegral);

        float ndotl_env = dot(worldNormal, envSample.direction);
        if (ndotl_env > 0.0f)
        {
            RayDesc shadowRayEnv;
            shadowRayEnv.Origin = worldPos + worldNormal * 0.001f;
            shadowRayEnv.Direction = envSample.direction;
            shadowRayEnv.TMin = 0.01f;
            shadowRayEnv.TMax = 10000.0f;

            ShadowPayload shadowPayloadEnv;
            shadowPayloadEnv.isHit = true;
            TraceRay(Scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER, 0xFF, 0, 1, 1, shadowRayEnv, shadowPayloadEnv);

            if (!shadowPayloadEnv.isHit)
            {
                float3 H = normalize(V + envSample.direction);
                float NDF = DistributionGGX(worldNormal, H, roughness);
                float G = GeometrySmith(worldNormal, V, envSample.direction, roughness);
                float3 F = FresnelSchlick(max(dot(H, V), 0.0f), F0);

                float3 numerator = NDF * G * F;
                float denominator = 4.0f * max(dot(worldNormal, V), 0.0f) * ndotl_env + 0.0001f;
                float3 specular = numerator / denominator;

                float3 kD = (1.0f - F) * (1.0f - metallic);
                float3 diff = kD * baseColor / PI;

                // --- MIS 評估：計算 BRDF PDF ---
                float G1 = GeometrySchlickGGX(max(dot(worldNormal, V), 0.0f), roughness);
                float pdf_specular = (NDF * G1) / (4.0f * max(dot(worldNormal, V), 0.0f) + 0.0001f);
                float pdf_diffuse = ndotl_env / PI;
                float pdf_brdf = lerp(pdf_diffuse, pdf_specular, specProbability);

                // Power Heuristic 公式: ENV² / (ENV² + BRDF²)
                float misWeight = (envSample.pdf * envSample.pdf) / (envSample.pdf * envSample.pdf + pdf_brdf * pdf_brdf);

                float3 neeContribDiffuse = diff * envSample.color * ndotl_env * misWeight / envSample.pdf;
                float3 neeContribSpecular = specular * envSample.color * ndotl_env * misWeight / envSample.pdf;

                if (payload.depth == 0)
                {
                    payload.diffuse += payload.throughput * neeContribDiffuse;
                    payload.specular += payload.throughput * neeContribSpecular;
                }
                else
                {
                    if (payload.isSpecularBounce)
                        payload.specular += payload.throughput * (neeContribDiffuse + neeContribSpecular);
                    else
                        payload.diffuse += payload.throughput * (neeContribDiffuse + neeContribSpecular);
                }
            }
        }
    }

    // ==========================================
    // 3. 計算間接光照與材質反射 (Indirect Bounce)
    // ==========================================
    if (payload.depth < 2)
    {
        bool isFirstBounce = (payload.depth == 0); // ← 必須在 depth++ 之前
        payload.depth++;

        float randomVal = rand(payload.seed);

        if (transmissionFactor > 0.0f)
        {
            // Schlick Fresnel 近似：掠射角反射率高，正入射幾乎全折射
            float cosTheta = abs(dot(-WorldRayDirection(), worldNormal));
            float F_schlick = 0.04f + (1.0f - 0.04f) * pow(1.0f - cosTheta, 5.0f);
            float fresnelReflect = F_schlick; // 只考慮表面反射率，不乘 transmissionFactor

            if (randomVal < fresnelReflect)
            {
                // ── 鏡面反射 ──
                if (isFirstBounce)
                    payload.isSpecularBounce = true;

                float alpha = max(roughness * roughness, 0.001f);
                float2 u = float2(rand(payload.seed), rand(payload.seed));
                float3x3 tbn = GetTBN(worldNormal);
                float3 V_local = normalize(mul(tbn, V));
                float3 H_local = SampleGGXVNDF(V_local, alpha, alpha, u.x, u.y);
                float3 H = normalize(mul(H_local, tbn));
                float3 rDir = reflect(-V, H);

                float G1 = GeometrySchlickGGX(max(dot(worldNormal, V), 0.0f), roughness);
                float G2 = GeometrySmith(worldNormal, V, rDir, roughness);
                float G_weight = (G1 > 0.0001f) ? (G2 / G1) : 0.0f;
                payload.throughput *= (F_bounce * G_weight) / max(fresnelReflect, 0.001f);

                if (dot(rDir, worldNormal) > 0.0f)
                {
                    RayDesc bounceRay;
                    bounceRay.Origin = worldPos + worldNormal * 0.001f;
                    bounceRay.Direction = rDir;
                    bounceRay.TMin = 0.01f;
                    bounceRay.TMax = 10000.0f;
                    TraceRay(Scene, RAY_FLAG_NONE, 0xFF, 0, 1, 0, bounceRay, payload);
                }
            }
            else
            {
                // ── 折射穿透 ──
                if (isFirstBounce)
                    payload.isSpecularBounce = false;

                float3 incomingDir = WorldRayDirection();
                float cosI = dot(-incomingDir, worldNormal);
                bool entering = (cosI > 0.0f);
                float eta = entering ? (1.0f / ior) : ior;
                float3 refractNormal = entering ? worldNormal : -worldNormal;

                float3 refractDir = refract(incomingDir, refractNormal, eta);
                if (dot(refractDir, refractDir) < 0.001f)
                    refractDir = reflect(incomingDir, refractNormal);

                float refractProb = 1.0f - fresnelReflect;
                // baseColor tint：模擬介質吸收（Beer-Lambert 近似）
                payload.throughput *= (baseColor * transmissionFactor) / max(refractProb, 0.001f);

                RayDesc transRay;
                transRay.Origin = worldPos - refractNormal * 0.002f;
                transRay.Direction = normalize(refractDir);
                transRay.TMin = 0.01f;
                transRay.TMax = 10000.0f;
                TraceRay(Scene, RAY_FLAG_NONE, 0xFF, 0, 1, 0, transRay, payload);
            }
        }
        else
        {
            // ── 非 Transmission 材質：diffuse/specular bounce ──
            float3 bounceDir;

            if (randomVal < specProbability)
            {
                if (isFirstBounce)
                    payload.isSpecularBounce = true;

                float alpha = max(roughness * roughness, 0.001f);
                float2 u = float2(rand(payload.seed), rand(payload.seed));
                float3x3 tbn = GetTBN(worldNormal);
                float3 V_local = normalize(mul(tbn, V));
                float3 H_local = SampleGGXVNDF(V_local, alpha, alpha, u.x, u.y);
                float3 H = normalize(mul(H_local, tbn));
                bounceDir = reflect(-V, H);

                float G1 = GeometrySchlickGGX(max(dot(worldNormal, V), 0.0f), roughness);
                float G2 = GeometrySmith(worldNormal, V, bounceDir, roughness);
                float G_weight = (G1 > 0.0001f) ? (G2 / G1) : 0.0f;
                payload.throughput *= (F_bounce * G_weight) / specProbability;
            }
            else
            {
                if (isFirstBounce)
                    payload.isSpecularBounce = false;
                bounceDir = getCosineWeightedSample(worldNormal, payload.seed);
                float3 kD = (1.0f - F_bounce) * (1.0f - metallic);
                payload.throughput *= (kD * baseColor) / (1.0f - specProbability);
            }

            if (dot(bounceDir, worldNormal) > 0.0f)
            {
                RayDesc bounceRay;
                bounceRay.Origin = worldPos + worldNormal * 0.001f;
                bounceRay.Direction = bounceDir;
                bounceRay.TMin = 0.01f;
                bounceRay.TMax = 10000.0f;
                
                // --- 準備發射反彈射線之前 ---
                float maxThroughput = max(payload.throughput.r, max(payload.throughput.g, payload.throughput.b));
                if (maxThroughput < 0.1f)
                {
                    float survivalProbability = max(maxThroughput, 0.05f);
                    if (rand(payload.seed) > survivalProbability)
                    {
                        return; // 提早終止這條光線
                    }
                    payload.throughput /= survivalProbability; // 能量補償
                }
                
                if (transmissionFactor > 0.0f)
                {
                    // 玻璃與折射視為純 Delta Function 鏡面
                    payload.lastRayPdf = 1.0f;
                    payload.lastRoughness = 0.0f;
                }
                else
                {
                    // 一般材質：評估這條反射路徑的 BRDF PDF
                    float pdf_specular_bounce = 0.0f;
                    float3 bounceH = normalize(V + bounceDir);
                    if (dot(worldNormal, bounceDir) > 0.0f)
                    {
                        float bounceD = DistributionGGX(worldNormal, bounceH, roughness);
                        float bounceG1 = GeometrySchlickGGX(max(dot(worldNormal, V), 0.0f), roughness);
                        pdf_specular_bounce = (bounceD * bounceG1) / (4.0f * max(dot(V, worldNormal), 0.0f) + 0.0001f);
                    }
                    float pdf_diffuse_bounce = max(dot(worldNormal, bounceDir), 0.0f) / PI;
        
                    payload.lastRayPdf = lerp(pdf_diffuse_bounce, pdf_specular_bounce, specProbability);
                    payload.lastRoughness = roughness;
                }
                
                TraceRay(Scene, RAY_FLAG_NONE, 0xFF, 0, 1, 0, bounceRay, payload);
            }
        }
    }
}