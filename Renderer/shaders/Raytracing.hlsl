#include "PBRCommon.hlsli"

RaytracingAccelerationStructure Scene : register(t0, space0);
RWTexture2D<float4> DiffuseTarget : register(u0, space0);
RWTexture2D<float4> SpecularTarget : register(u1, space0);
Texture2D<float4> EnvMap : register(t1, space0);
Buffer<float> EnvMarginalCDF : register(t2, space0); // 周辺確率分布 (1D)
Buffer<float> EnvConditionalCDF : register(t3, space0); // 条件付き確率分布 (2D)

cbuffer CameraParams : register(b0, space0)
{
    float4x4 viewProjInv;
    float3 cameraPos;
    uint frameCount;
    float envIntegral; // 環境光の総エネルギー (PDF 計算用)
    float jitterX;     // (予約) NDC オフセット
    float jitterY;
    float _padCamera;
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
    float roughnessFactor;
    float metallicFactor;
    uint _matPad;
};

Texture2D allTextures[] : register(t0, space2);
SamplerState texSampler : register(s0, space0);

static const float INV_PI = 0.318309886f;
static const float INV_TWO_PI = 0.159154943f;

// ==========================================
// 乱数とサンプリングツール
// ==========================================

// 3x3 空間内で非常に均一に分布する低差異ノイズを生成、SVGF デノイズに特に適している
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

    // 接線空間 (Tangent Space) を構築
    float3 up = abs(n.z) < 0.999f ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 t = normalize(cross(up, n));
    float3 b = cross(n, t);

    return normalize(t * x + b * y + n * z);
}

// 3D 方向ベクトルを 2D 球面 UV 座標に変換
float2 GetSphericalUV(float3 v)
{
    // Y 軸上向き (Y-Up) と仮定
    // atan2 で水平経度 [-PI, PI] を取得 -> u
    // asin で垂直緯度 [-PI/2, PI/2] を取得 -> v
    float2 uv = float2(atan2(v.z, v.x), asin(v.y));

    // [-0.5, 0.5] にマッピング
    uv *= float2(INV_TWO_PI, INV_PI);

    // [0, 1] にマッピング
    uv += 0.5f;

    // DirectX の V 座標は上端が起点なので V を反転
    uv.y = 1.0f - uv.y;
    
    return uv;
}

// ==========================================
// HDRI 補助検索とサンプリング (NEE コア)
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

    // 二分探索で対応する高輝度ピクセル位置を取得
    uint y = FindIntervalMarginal(u.y, EnvHeight);
    uint x = FindIntervalConditional(y, u.x, EnvWidth);
    float2 uv = float2((x + 0.5f) / EnvWidth, (y + 0.5f) / EnvHeight);

    // UV を 3D 空間方向に逆変換 (GetSphericalUV の逆解き)
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
    // 輝度を総エネルギーで割り、その方向の確率密度 (PDF) を求める
    res.pdf = max(luma / max(envInt, 0.0001f), 0.0001f);
    return res;
}

// ==========================================
// Payloads
// ==========================================
struct Payload
{
    float3 diffuse; // 収集された拡散反射エネルギー
    uint depth; // 現在のバウンス回数
    float3 specular; // 収集された鏡面反射エネルギー
    uint seed; // 乱数シード
    float3 throughput; // 光線の減衰ウェイト
    bool isSpecularBounce; // 最初のバウンスで鏡面反射パスを通ったかを示すフラグ
    float lastRayPdf; // 前の光線の BRDF 確率密度
    float lastRoughness; // 前の反射面の粗さ
    float coneWidth; // 4 bytes
    float coneSpreadAngle; // 4 bytes
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

    // NEE で天空をサンプリングした場合の確率密度を計算
    float luma = dot(skyColor, float3(0.2126f, 0.7152f, 0.0722f));
    float pdf_env = max(luma / max(envIntegral, 0.0001f), 0.0001f);

    float misWeight = 1.0f;
    
    // 間接バウンス光線の場合、MIS を実行
    if (payload.depth > 0)
    {
        if (payload.lastRoughness < 0.05f)
        {
            // 非常に滑らかな表面 (鏡面に近い)、NEE での能動的なサンプリングが困難で BRDF ブラインドシューティングに完全依存
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
    
    float fovRadians = 45.0f * (3.14159265f / 180.0f);
    float pixelSpreadAngle = atan(tan(fovRadians * 0.5f) * 2.0f / (float) launchDim.y);

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
        payload.coneWidth = 0.0f;
        payload.coneSpreadAngle = pixelSpreadAngle;
        
        payload.seed = pcg_hash(linearIndex ^ (frameCount * 114514u) ^ (s * 1973u));

        RayDesc ray;
        ray.Origin = cameraPos;
        ray.Direction = rayDir;
        ray.TMin = 0.001f;
        ray.TMax = 10000.0f;

        TraceRay(Scene, RAY_FLAG_NONE, 0xFF, 0, 2, 0, ray, payload);

        // ホタル抑制 (Firefly Clamping)
        // 20.0 → 6.0 に強化。HDRI の sun ピクセル直撃が denoiser 管線を汚染するのを防ぐ。
        // 下流 (TemporalAccumulation) の上限と一致させ、中央値計算で生き残るホタルを根絶する。
        const float MAX_RADIANCE = 6.0f;

        float lumaD = dot(payload.diffuse, float3(0.2126f, 0.7152f, 0.0722f));
        if (lumaD > MAX_RADIANCE) 
            payload.diffuse *= (MAX_RADIANCE / lumaD);

        float lumaS = dot(payload.specular, float3(0.2126f, 0.7152f, 0.0722f));
        if (lumaS > MAX_RADIANCE) 
            payload.specular *= (MAX_RADIANCE / lumaS);

        accumDiffuse += payload.diffuse;
        accumSpecular += payload.specular;
    }

    // 2 枚のテクスチャに個別出力
    DiffuseTarget[launchIndex] = float4(accumDiffuse / (float) SPP, 1.0f);
    SpecularTarget[launchIndex] = float4(accumSpecular / (float) SPP, 1.0f);
}
[shader("closesthit")]
void ClosestHit(inout Payload payload, in BuiltInTriangleIntersectionAttributes attr)
{
    uint primitiveIndex = PrimitiveIndex();
    
    // Index Buffer を通して 3 頂点のインデックスを取得
    uint i0 = IndexBuffer.Load(primitiveIndex * 12 + 0);
    uint i1 = IndexBuffer.Load(primitiveIndex * 12 + 4);
    uint i2 = IndexBuffer.Load(primitiveIndex * 12 + 8);
    
    float3 n0 = VertexBuffer[i0].normal;
    float3 n1 = VertexBuffer[i1].normal;
    float3 n2 = VertexBuffer[i2].normal;
    
    float2 uv0 = VertexBuffer[i0].uv;
    float2 uv1 = VertexBuffer[i1].uv;
    float2 uv2 = VertexBuffer[i2].uv;
    
    // 重心座標を使って補間
    float3 barycentrics = float3(1.0f - attr.barycentrics.x - attr.barycentrics.y, attr.barycentrics.x, attr.barycentrics.y);
    float3 localNormal = normalize(n0 * barycentrics.x + n1 * barycentrics.y + n2 * barycentrics.z);
    float2 localUV = uv0 * barycentrics.x + uv1 * barycentrics.y + uv2 * barycentrics.z;
    
    float3x4 mInv = WorldToObject3x4();
    float3 worldNormal = normalize(mul(localNormal, (float3x3) mInv));
    float3 geoNormal = worldNormal;
    float3 worldPos = WorldRayOrigin() + WorldRayDirection() * RayTCurrent();
    
    // カメラの視線方向
    float3 V = normalize(cameraPos - worldPos);
    
    // Mipmap LOD 計算 (Ray Cone)
    float hitT = RayTCurrent();
    float currentConeWidth = payload.coneWidth + (payload.coneSpreadAngle * hitT);
    float lod = 0.0f;

    if (textureIndex != 0xFFFFFFFF)
    {
        // 1. Object Space の頂点を取得し ObjectToWorld で World Space ベクトルに変換
        float3 p0 = VertexBuffer[i0].position;
        float3 p1 = VertexBuffer[i1].position;
        float3 p2 = VertexBuffer[i2].position;

        float3x4 mO2W = ObjectToWorld3x4();
        float3 worldE1 = mul((float3x3) mO2W, p1 - p0);
        float3 worldE2 = mul((float3x3) mO2W, p2 - p0);
        
        // World Space 面積
        float worldArea = length(cross(worldE1, worldE2)) * 0.5f;

        // UV Space 面積
        float uvArea = abs((uv1.x - uv0.x) * (uv2.y - uv0.y) - (uv2.x - uv0.x) * (uv1.y - uv0.y)) * 0.5f;

        // 現在のテクスチャの幅と高さを動的に取得
        uint texWidth, texHeight;
        allTextures[NonUniformResourceIndex(textureIndex)].GetDimensions(texWidth, texHeight);

        // カバーピクセルを計算して LOD を導出
        float footprintArea = 3.14159265f * currentConeWidth * currentConeWidth;
        float uvFootprintArea = (footprintArea / max(worldArea, 1e-8f)) * uvArea;
        float texAreaPixels = uvFootprintArea * (float) texWidth * (float) texHeight;

        lod = max(0.0f, 0.5f * log2(max(texAreaPixels, 1e-8f)));
    }

    // ==========================================
    // 1. PBR テクスチャをサンプリング (Albedo、Roughness、Metallic)
    // ==========================================
    float3 baseColor = float3(0.9f, 0.9f, 0.9f);
    // transmission 材質はミラーガラスまで下げられるよう roughness 下限を 0 に緩める
    float roughnessMin = (transmissionFactor > 0.0f) ? 0.0f : 0.05f;
    float roughness = clamp(roughnessFactor, roughnessMin, 1.0f);
    float metallic  = saturate(metallicFactor);

    float3 baseColorFactorLinear = float3(baseColorFactor_r, baseColorFactor_g, baseColorFactor_b);

    if (textureIndex != 0xFFFFFFFF)
    {
        // テクスチャあり：テクスチャ色 * baseColorFactor
        float4 texColor = allTextures[NonUniformResourceIndex(textureIndex)].SampleLevel(texSampler, localUV, lod);
        baseColor = pow(texColor.rgb, 2.2f) * baseColorFactorLinear;

        // glTF 仕様: 最終値 = テクスチャ値 * factor
        float4 mrColor = allTextures[NonUniformResourceIndex(textureIndex + 1)].SampleLevel(texSampler, localUV, lod);
        roughness = clamp(mrColor.g * roughnessFactor, roughnessMin, 1.0f);
        metallic  = saturate(mrColor.b * metallicFactor);
        
        float3 localNormalMap = allTextures[NonUniformResourceIndex(textureIndex + 2)].SampleLevel(texSampler, localUV, lod).xyz;
        localNormalMap = localNormalMap * 2.0f - 1.0f; // [-1, 1] に変換

        // Y 軸を反転して glTF (OpenGL) と DX12 の法線システムの差異を修正
        localNormalMap.y = -localNormalMap.y;

        float3 e1 = VertexBuffer[i1].position - VertexBuffer[i0].position;
        float3 e2 = VertexBuffer[i2].position - VertexBuffer[i0].position;
        float2 duv1 = uv1 - uv0;
        float2 duv2 = uv2 - uv0;
        
        float det = duv1.x * duv2.y - duv1.y * duv2.x;
        float3 worldTangent;
        float r = 1.0f / det;
        
        // 【フェイルセーフ】UV の縮退によるゼロ除算 (NaN) を防ぐ
        if (abs(det) < 1e-6f)
        {
            // 有効な UV がない場合、クラッシュを防ぐためデフォルトの接線を設定
            float3 up = abs(worldNormal.z) < 0.999f ? float3(0, 0, 1) : float3(1, 0, 0);
            worldTangent = normalize(cross(up, worldNormal));
        }
        else
        {
            float3 objTangent = (e1 * duv2.y - e2 * duv1.y) * r;
            float3x4 mO2W = ObjectToWorld3x4();
            worldTangent = normalize(mul((float3x3) mO2W, objTangent));
        }
        
        // 正規直交化を確保
        worldTangent = normalize(worldTangent - dot(worldTangent, worldNormal) * worldNormal);
        // r の符号を使ってミラー UV の従接線方向を修正
        float handedness = (r < 0.0f) ? -1.0f : 1.0f;
        float3 worldBitangent = cross(worldNormal, worldTangent) * handedness;
        
        // 元の滑らかな worldNormal を上書きし、後続の屈折・反射に凹凸の細部を加える！
        float3x3 tbn = float3x3(worldTangent, worldBitangent, worldNormal);
        worldNormal = normalize(mul(localNormalMap, tbn));
    }
    else
    {
        // テクスチャなし (例: PawnTopWhite)：baseColorFactor を直接使用
        baseColor = baseColorFactorLinear;
    }

    // 基礎反射率：非金属のデフォルトは 4%、金属は元の色を持つ
    float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), baseColor, metallic);

    // ==========================================
    // 2. 直接照明を計算 (Direct Illumination + GGX Specular)
    // ==========================================
    float3 directLighting = float3(0, 0, 0);

    for (int i = 0; i < numLights; i++)
    {
        Light L = lights[i];
        float3 lightDir;
        float attenuation = 1.0f;
        float shadowTMax = 10000.0f;

        if (L.type == 0) // 平行光 (Directional Light)
        {
            lightDir = normalize(-L.direction);
        }
        else // 点光源 (Point Light)
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
            // シャドウ判定
            RayDesc shadowRay;
            shadowRay.Origin = worldPos + worldNormal * 0.001f;
            shadowRay.Direction = lightDir;
            shadowRay.TMin = 0.01f;
            shadowRay.TMax = shadowTMax;

            ShadowPayload shadowPayload;
            shadowPayload.isHit = true;
            TraceRay(Scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER, 0xFF, 1, 2, 1, shadowRay, shadowPayload);

            if (!shadowPayload.isHit)
            {
                // --- PBR コア照明計算 ---
                float3 H = normalize(V + lightDir);
                float NDF = DistributionGGX(worldNormal, H, roughness);
                float G = GeometrySmith(worldNormal, V, lightDir, roughness);
                float3 F = FresnelSchlick(max(dot(H, V), 0.0f), F0);

                // 鏡面反射項 (Specular)
                float3 numerator = NDF * G * F;
                float denominator = 4.0f * max(dot(worldNormal, V), 0.0f) * ndotl + 0.0001f;
                float3 specular = numerator / denominator;

                // 拡散反射項 (Diffuse)
                // 透過材質 (ガラス等) は Lambertian 拡散を持たない。
                // (1-F) のエネルギーは吸収される代わりに屈折光として媒質内に入るため、
                // diffuse チャンネルに書き込むと不透明な Lambertian 像として見えてしまう。
                float3 kD = (1.0f - F) * (1.0f - metallic) * (1.0f - transmissionFactor);
                float3 diff = (payload.depth == 0) ? (kD / PI) : (kD * baseColor / PI);

                float3 currentDirectDiffuse = diff * L.color * L.intensity * ndotl * attenuation;
                float3 currentDirectSpecular = specular * L.color * L.intensity * ndotl * attenuation;

                // 直接照明を正確に分離
                if (payload.depth == 0)
                {
                    payload.diffuse += payload.throughput * currentDirectDiffuse;
                    payload.specular += payload.throughput * currentDirectSpecular;
                }
                else
                {
                    // 後続バウンスの場合、最初のバウンスの属性に基づいてエネルギーを分類
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
    // 2.5 環境光直接サンプリング (Next Event Estimation + MIS)
    // ==========================================
    // ある程度の粗さを持つ表面に対して、HDRI 内のハイライト点を積極的に探す
    if (roughness >= 0.05f)
    {
        float2 uEnv = float2(rand(payload.seed), rand(payload.seed));
        EnvSampleResult envSample = SampleEnv(uEnv, envIntegral);

        float3 neeNormal = (dot(V, worldNormal) >= 0.0f) ? worldNormal : -worldNormal;
        float ndotl_env = dot(neeNormal, envSample.direction);
        if (ndotl_env > 0.0f)
        {
            RayDesc shadowRayEnv;
            float3 neeOriginOffset = (dot(V, worldNormal) >= 0.0f) ? worldNormal : -worldNormal;
            shadowRayEnv.Origin = worldPos + neeOriginOffset * 0.001f;
            shadowRayEnv.Direction = envSample.direction;
            shadowRayEnv.TMin = 0.01f;
            shadowRayEnv.TMax = 10000.0f;

            ShadowPayload shadowPayloadEnv;
            shadowPayloadEnv.isHit = true;
            TraceRay(Scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER, 0xFF, 1, 2, 1, shadowRayEnv, shadowPayloadEnv);

            if (!shadowPayloadEnv.isHit)
            {
                float3 H = normalize(V + envSample.direction);
                float NDF = DistributionGGX(worldNormal, H, roughness);
                float G = GeometrySmith(worldNormal, V, envSample.direction, roughness);
                float3 F = FresnelSchlick(max(dot(H, V), 0.0f), F0);

                float3 numerator = NDF * G * F;
                float denominator = 4.0f * max(dot(worldNormal, V), 0.0f) * ndotl_env + 0.0001f;
                float3 specular = numerator / denominator;

                float3 kD = (1.0f - F) * (1.0f - metallic) * (1.0f - transmissionFactor);
                float3 diff = (payload.depth == 0) ? (kD / PI) : (kD * baseColor / PI);

                // --- MIS 評価：BRDF PDF を計算 ---
                float G1 = GeometrySchlickGGX(max(dot(worldNormal, V), 0.0f), roughness);
                float pdf_spec = (NDF * G1) / (4.0f * max(dot(worldNormal, V), 0.0f) + 0.0001f);
                float pdf_diff = ndotl_env / PI;
                float pdf_brdf = lerp(pdf_diff, pdf_spec, specProbability);

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
    // 3. 間接照明とマテリアル反射を計算 (Indirect Bounce)
    // ==========================================
    if (payload.depth < 2)
    {
        // Ray Cone パラメータを次のバウンスのために更新
        payload.coneWidth = currentConeWidth;
        payload.coneSpreadAngle += roughness * 0.1f; // 表面の粗さにより後続バウンス光線が発散する
        
        bool isFirstBounce = (payload.depth == 0); // ← depth++ の前に行う必要がある
        payload.depth++;

        float randomVal = rand(payload.seed);

        if (transmissionFactor > 0.0f)
        {
            // Schlick Fresnel 近似：グレージング角で反射率が高く、正入射ではほぼ全屈折
            float cosTheta = abs(dot(-WorldRayDirection(), worldNormal));
            float f0_scalar = (1.0f - ior) / (1.0f + ior);
            f0_scalar *= f0_scalar;
            float F_schlick = f0_scalar + (1.0f - f0_scalar) * pow(1.0f - cosTheta, 5.0f);
            float fresnelReflect = F_schlick; // 表面反射率のみを考慮し、transmissionFactor は乗算しない

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
                payload.throughput *= G_weight / max(fresnelReflect, 0.001f);

                if (dot(rDir, worldNormal) > 0.0f)
                {
                    float maxThroughput = max(payload.throughput.r, max(payload.throughput.g, payload.throughput.b));
                    if (maxThroughput < 0.1f)
                    {
                        float survivalProb = max(maxThroughput, 0.05f);
                        if (rand(payload.seed) > survivalProb)
                            return; // 早期終了
                        payload.throughput /= survivalProb; // エネルギー補償
                    }

                    RayDesc bounceRay;
                    bounceRay.Origin = worldPos + worldNormal * 0.001f;
                    bounceRay.Direction = rDir;
                    bounceRay.TMin = 0.01f;
                    bounceRay.TMax = 10000.0f;
                    TraceRay(Scene, RAY_FLAG_NONE, 0xFF, 0, 2, 0, bounceRay, payload);
                }
            }
            else
            {
                // ── 屈折透過 ──
                if (isFirstBounce)
                    payload.isSpecularBounce = true;

                float3 incomingDir = WorldRayDirection();

                // 屈折も滑らかなシェーディング法線を使う
                // ただし TIR / 面裏判定の安定性のため geoNormal で進入判定
                float cosI_geo = dot(-incomingDir, geoNormal);
                bool entering = (cosI_geo > 0.0f);

                // シェーディング法線も向きを進行方向側に揃える
                float3 Ns = worldNormal;
                if (dot(Ns, geoNormal) < 0.0f)
                    Ns = -Ns; // まれに反転していたら直す
                float3 refractNormal = entering ? Ns : -Ns;

                float eta = entering ? (1.0f / ior) : ior;

                float3 refractDir = refract(incomingDir, refractNormal, eta);
                if (dot(refractDir, refractDir) < 0.001f)
                    refractDir = reflect(incomingDir, refractNormal);

                float refractProb = 1.0f - fresnelReflect;
                float pathLength = RayTCurrent(); // 光線が媒質内を進んだ距離

                // baseColor を吸収係数に変換：色が暗いほど吸収が速い
                // epsilon を加えて log(0) を防ぐ — 純白は吸収なし、純黒は極めて強い吸収
                float3 beerLambert = float3(1, 1, 1);
                if (!entering)
                {
                    float3 absorptionCoeff = -log(max(baseColor, 0.001f));
                    beerLambert = exp(-absorptionCoeff * pathLength);
                }
                payload.throughput *= (beerLambert * transmissionFactor) / max(refractProb, 0.001f);
                
                float maxThroughput = max(payload.throughput.r, max(payload.throughput.g, payload.throughput.b));
                if (maxThroughput < 0.1f)
                {
                    float survivalProb = max(maxThroughput, 0.05f);
                    if (rand(payload.seed) > survivalProb)
                        return; // 早期終了
                    payload.throughput /= survivalProb; // エネルギー補償
                }

                RayDesc transRay;
                transRay.Origin = worldPos - refractNormal * 0.002f;
                transRay.Direction = normalize(refractDir);
                transRay.TMin = 0.01f;
                transRay.TMax = 10000.0f;
                TraceRay(Scene, RAY_FLAG_NONE, 0xFF, 0, 2, 0, transRay, payload);
            }
        }
        else
        {
            // ── 非 Transmission マテリアル：diffuse/specular bounce ──
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
                if (payload.depth == 0)
                    payload.throughput *= kD / (1.0f - specProbability);
                else
                    payload.throughput *= (kD * baseColor) / (1.0f - specProbability);
            }

            if (dot(bounceDir, worldNormal) > 0.0f)
            {
                RayDesc bounceRay;
                bounceRay.Origin = worldPos + worldNormal * 0.001f;
                bounceRay.Direction = bounceDir;
                bounceRay.TMin = 0.01f;
                bounceRay.TMax = 10000.0f;
                
                // --- バウンス光線を発射する前の準備 ---
                float maxThroughput = max(payload.throughput.r, max(payload.throughput.g, payload.throughput.b));
                if (maxThroughput < 0.1f)
                {
                    float survivalProbability = max(maxThroughput, 0.05f);
                    if (rand(payload.seed) > survivalProbability)
                    {
                        return; // この光線を早期終了
                    }
                    payload.throughput /= survivalProbability; // エネルギー補償
                }
                
                if (transmissionFactor > 0.0f)
                {
                    // ガラスと屈折は純粋な Delta Function 鏡面として扱う
                    payload.lastRayPdf = 1.0f;
                    payload.lastRoughness = 0.0f;
                }
                else
                {
                    // 通常マテリアル：この反射パスの BRDF PDF を評価
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
                
                TraceRay(Scene, RAY_FLAG_NONE, 0xFF, 0, 2, 0, bounceRay, payload);
            }
        }
    }
}

// ==========================================
// Any Hit Shaders (Alpha Cutout / Mask)
// ==========================================

[shader("anyhit")]
void AnyHit(inout Payload payload, in BuiltInTriangleIntersectionAttributes attr)
{
    if (textureIndex == 0xFFFFFFFF)
        return;

    uint primitiveIndex = PrimitiveIndex();
    uint i0 = IndexBuffer.Load(primitiveIndex * 12 + 0);
    uint i1 = IndexBuffer.Load(primitiveIndex * 12 + 4);
    uint i2 = IndexBuffer.Load(primitiveIndex * 12 + 8);
    
    float2 uv0 = VertexBuffer[i0].uv;
    float2 uv1 = VertexBuffer[i1].uv;
    float2 uv2 = VertexBuffer[i2].uv;
    
    float3 barycentrics = float3(1.0f - attr.barycentrics.x - attr.barycentrics.y, attr.barycentrics.x, attr.barycentrics.y);
    float2 localUV = uv0 * barycentrics.x + uv1 * barycentrics.y + uv2 * barycentrics.z;
    
    // Base Color テクスチャの Alpha チャンネルをサンプリング
    float alpha = allTextures[NonUniformResourceIndex(textureIndex)].SampleLevel(texSampler, localUV, 0).a;
    alpha *= baseColorFactor_a;
    
    if (alpha < 0.5f)
    {
        IgnoreHit(); // 透明領域、この衝突を無視
    }
}

[shader("anyhit")]
void ShadowAnyHit(inout ShadowPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
    if (textureIndex == 0xFFFFFFFF)
        return;

    uint primitiveIndex = PrimitiveIndex();
    uint i0 = IndexBuffer.Load(primitiveIndex * 12 + 0);
    uint i1 = IndexBuffer.Load(primitiveIndex * 12 + 4);
    uint i2 = IndexBuffer.Load(primitiveIndex * 12 + 8);
    
    float2 uv0 = VertexBuffer[i0].uv;
    float2 uv1 = VertexBuffer[i1].uv;
    float2 uv2 = VertexBuffer[i2].uv;
    
    float3 barycentrics = float3(1.0f - attr.barycentrics.x - attr.barycentrics.y, attr.barycentrics.x, attr.barycentrics.y);
    float2 localUV = uv0 * barycentrics.x + uv1 * barycentrics.y + uv2 * barycentrics.z;
    
    float alpha = allTextures[NonUniformResourceIndex(textureIndex)].SampleLevel(texSampler, localUV, 0).a;
    alpha *= baseColorFactor_a;
    
    if (alpha < 0.5f)
    {
        IgnoreHit();
    }
}