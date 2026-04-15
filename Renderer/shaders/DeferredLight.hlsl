#include "PBRCommon.hlsli"
#include "LightDef.hlsli"

struct VSOutput
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD;
};

// SV_VertexID を利用してフルスクリーン三角形を自動生成
VSOutput VSMain(uint vertexID : SV_VertexID)
{
    VSOutput output;
    output.uv = float2((vertexID << 1) & 2, vertexID & 2);
    output.pos = float4(output.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return output;
}

Texture2D g_Albedo : register(t0);
Texture2D g_NormalRoughness : register(t1);
Texture2D g_WorldPosMetallic : register(t2);
SamplerState g_sampler : register(s0);

float4 PSMain(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    float4 albedoTex = g_Albedo.Sample(g_sampler, uv);
    float4 normalRoughness = g_NormalRoughness.Sample(g_sampler, uv);
    float4 worldPosMetallic = g_WorldPosMetallic.Sample(g_sampler, uv);

    // 背景の場合 (法線の長さが 0、またはカスタムフラグ)、背景色を直接出力
    if (length(normalRoughness.rgb) < 0.1f)
    {
        return float4(0.2f, 0.2f, 0.2f, 1.0f);
    }

    float3 albedo = albedoTex.rgb;
    float roughness = normalRoughness.a;
    float3 N = normalize(normalRoughness.rgb);
    float metallic = worldPosMetallic.a;
    float3 P = worldPosMetallic.xyz;
    float3 V = normalize(cameraPos - P);
    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo.rgb, metallic);

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
            LDir = normalize(l.position - P); // Forward では v.worldPos に相当
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