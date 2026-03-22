static const float PI = 3.14159265359;

cbuffer SceneConstants : register(b0)
{
    float4x4 mvp;
    float4x4 normalMatrix;
    float3 lightDir;
    float _pad1;
    float3 cameraPos;
    float _pad2;
    float4 baseColor;
};

struct VSIn
{
    float3 pos : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

struct VSOut
{
    float4 pos : SV_POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float3 worldPos : TEXCOORD1; // 新增：世界座標
};

VSOut VSMain(VSIn v)
{
    VSOut o;
    o.pos = mul(float4(v.pos, 1.0), mvp);
    o.normal = normalize(mul(v.normal, (float3x3) normalMatrix));
    o.uv = v.uv;
    o.worldPos = v.pos; // 假設 Model 矩陣是 Identity，直接取 pos
    return o;
}

Texture2D g_texture : register(t0); // BaseColor
Texture2D g_metallicRoughness : register(t1); // Metallic Roughness
SamplerState g_sampler : register(s0);

// --- PBR 輔助數學函式 ---
float DistributionGGX(float3 N, float3 H, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    float num = a2;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;
    return num / max(denom, 0.0000001);
}

float GeometrySchlickGGX(float NdotV, float roughness)
{
    float r = (roughness + 1.0);
    float k = (r * r) / 8.0;
    float num = NdotV;
    float denom = NdotV * (1.0 - k) + k;
    return num / denom;
}

float GeometrySmith(float3 N, float3 V, float3 L, float roughness)
{
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float ggx2 = GeometrySchlickGGX(NdotV, roughness);
    float ggx1 = GeometrySchlickGGX(NdotL, roughness);
    return ggx1 * ggx2;
}

float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// --- 主要 Pixel Shader ---
float4 PSMain(VSOut v) : SV_TARGET
{
    float4 albedoTex = g_texture.Sample(g_sampler, v.uv);
    clip(albedoTex.a - 0.5f); // 處理 Sponza 的葉子鏤空
    
    // 從 G 通道拿粗糙度，B 通道拿金屬度
    float4 mrTex = g_metallicRoughness.Sample(g_sampler, v.uv);
    float roughness = mrTex.g;
    float metallic = mrTex.b;

    // 物理向量
    float3 N = normalize(v.normal);
    float3 V = normalize(cameraPos - v.worldPos);
    float3 L = normalize(-lightDir);
    float3 H = normalize(V + L);
    
    float NdotL = max(dot(N, L), 0.0);

    // 基礎反射率 (非金屬預設 0.04，金屬則繼承 BaseColor)
    float3 F0 = float3(0.04, 0.04, 0.04);
    F0 = lerp(F0, albedoTex.rgb, metallic);

    // 1. 計算高光 BRDF (Cook-Torrance)
    float NDF = DistributionGGX(N, H, roughness);
    float G = GeometrySmith(N, V, L, roughness);
    float3 F = FresnelSchlick(max(dot(H, V), 0.0), F0);

    float3 numerator = NDF * G * F;
    float denominator = 4.0 * max(dot(N, V), 0.0) * NdotL;
    float3 specular = numerator / max(denominator, 0.001);

    // 2. 計算漫反射 (能量守恆)
    float3 kS = F;
    float3 kD = float3(1.0, 1.0, 1.0) - kS;
    kD *= 1.0 - metallic; // 金屬沒有漫反射

    // 3. 組合最終光照
    float3 radiance = float3(3.0, 3.0, 3.0); // 光源強度
    float3 Lo = (kD * albedoTex.rgb / PI + specular) * radiance * NdotL;

    // 加上基礎環境光，避免背光處純黑
    float3 ambient = float3(0.1, 0.1, 0.1) * albedoTex.rgb * albedoTex.a; // AO 通常存在 MR 貼圖的 R 通道，這裡簡單模擬
    float3 color = ambient + Lo;

    // HDR 色調映射與 Gamma 校正 (ACES Film 簡化版)
    color = color / (color + float3(1.0, 1.0, 1.0));
    color = pow(color, float3(1.0 / 2.2, 1.0 / 2.2, 1.0 / 2.2));

    return float4(color, albedoTex.a);
}