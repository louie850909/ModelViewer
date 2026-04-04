#include "PBRCommon.hlsli"
#include "LightDef.hlsli"

cbuffer SceneConstants : register(b0)
{
    matrix mvp;
    matrix modelMatrix;
    matrix normalMatrix;
    float3 cameraPos;
    float _pad1;
    float3 lightDir;
    float _pad2;
    float4 baseColor;
};

struct VSOutput
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD;
};

// 利用 SV_VertexID 自動產生全螢幕三角形
VSOutput VSMain(uint vertexID : SV_VertexID)
{
    VSOutput output;
    output.uv = float2((vertexID << 1) & 2, vertexID & 2);
    output.pos = float4(output.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return output;
}

Texture2D g_AlbedoRough : register(t0);
Texture2D g_NormalMetal : register(t1);
Texture2D g_WorldPos : register(t2);
SamplerState g_sampler : register(s0);

float4 PSMain(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    float4 albedoRough = g_AlbedoRough.Sample(g_sampler, uv);
    float4 normalMetal = g_NormalMetal.Sample(g_sampler, uv);
    float4 worldPos = g_WorldPos.Sample(g_sampler, uv);

    // 如果是背景 (法線長度為0，或自訂標記)，直接輸出背景色
    if (length(normalMetal.rgb) < 0.1f)
    {
        return float4(0.2f, 0.2f, 0.2f, 1.0f);
    }

    float3 albedo = albedoRough.rgb;
    float roughness = albedoRough.a;
    float3 N = normalize(normalMetal.rgb);
    float metallic = normalMetal.a;
    float3 P = worldPos.xyz;

    float3 V = normalize(cameraPos - P);
    float3 L = normalize(-lightDir); // 注意方向
    float3 H = normalize(V + L);

    float NdotL = max(dot(N, L), 0.0);

    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo.rgb, metallic);

    float NDF = DistributionGGX(N, H, roughness);
    float G = GeometrySmith(N, V, L, roughness);
    float3 F = FresnelSchlick(max(dot(H, V), 0.0), F0);

    float3 numerator = NDF * G * F;
    float denominator = 4.0 * max(dot(N, V), 0.0) * NdotL;
    float3 specular = numerator / max(denominator, 0.001);

    float3 kD = (float3(1.0, 1.0, 1.0) - F) * (1.0 - metallic);

    float3 radiance = float3(3.0, 3.0, 3.0);
    float3 Lo = float3(0, 0, 0);
    for (int i = 0; i < numLights; i++)
    {
        Light l = lights[i];
        float3 LDir;
        float attenuation = 1.0;

        if (l.type == 0)
        { // Directional
            LDir = normalize(-l.direction);
        }
        else if (l.type == 1)
        { // Point
            LDir = normalize(l.position - P); // 此處在 Forward 中為 v.worldPos
            float dist = length(l.position - P);
            attenuation = 1.0 / (dist * dist + 0.0001);
        }
        else if (l.type == 2)
        { // Spot
            LDir = normalize(l.position - P);
            float dist = length(l.position - P);
            attenuation = 1.0 / (dist * dist + 0.0001);
            float theta = dot(LDir, normalize(-l.direction));
            float epsilon = cos(l.coneAngle * PI / 180.0) - cos((l.coneAngle + 10.0) * PI / 180.0);
            float spot = clamp((theta - cos((l.coneAngle + 10.0) * PI / 180.0)) / epsilon, 0.0, 1.0);
            attenuation *= spot;
        }

        float3 H = normalize(V + LDir);
        float NdotL = max(dot(N, LDir), 0.0);
        float NDF = DistributionGGX(N, H, roughness);
        float G = GeometrySmith(N, V, LDir, roughness);
        float3 F = FresnelSchlick(max(dot(H, V), 0.0), F0);

        float3 numerator = NDF * G * F;
        float denominator = 4.0 * max(dot(N, V), 0.0) * NdotL;
        float3 specular = numerator / max(denominator, 0.001);
        float3 kD = (float3(1.0, 1.0, 1.0) - F) * (1.0 - metallic);

        float3 radiance = l.color * l.intensity * attenuation;
        Lo += (kD * albedo.rgb / PI + specular) * radiance * NdotL;
    }

    float3 ambient = float3(0.1, 0.1, 0.1) * albedo.rgb;
    float3 color = ambient + Lo;

    color = color / (color + float3(1.0, 1.0, 1.0));
    color = pow(color, float3(1.0 / 2.2, 1.0 / 2.2, 1.0 / 2.2));

    return float4(color, 1);
}