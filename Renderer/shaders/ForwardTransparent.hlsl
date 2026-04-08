#include "PBRCommon.hlsli"
#include "LightDef.hlsli"

cbuffer PassConstants : register(b0)
{
    matrix viewProj;
    matrix unjitteredViewProj;
    matrix prevUnjitteredViewProj;
};

// 由於 LightDef.hlsli 已經佔用了 b1，我們把模型常數綁在 b2
cbuffer ObjectConstants : register(b2)
{
    matrix modelMatrix;
};

struct VSInput
{
    float3 pos : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD;
};

struct VSOutput
{
    float4 pos : SV_Position;
    float3 worldPos : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD;
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    float4 worldPos = mul(float4(input.pos, 1.0f), modelMatrix);
    output.worldPos = worldPos.xyz;
    output.pos = mul(worldPos, viewProj);
    float3x3 m3x3 = (float3x3) modelMatrix;
    float3x3 adj = transpose(float3x3(
        cross(m3x3[1], m3x3[2]),
        cross(m3x3[2], m3x3[0]),
        cross(m3x3[0], m3x3[1])
    ));
    output.normal = normalize(mul(input.normal, adj));
    output.uv = input.uv;
    return output;
}



Texture2D g_texture : register(t0);
Texture2D g_metallicRoughness : register(t1);
SamplerState g_sampler : register(s0);

float4 PSMain(VSOutput v) : SV_TARGET
{
    float4 albedoTex = g_texture.Sample(g_sampler, v.uv);
    if (albedoTex.a < 0.01f)
        discard;

    float4 mrTex = g_metallicRoughness.Sample(g_sampler, v.uv);
    float roughness = mrTex.g;
    float metallic = mrTex.b;

    float3 N = normalize(v.normal);
    float3 V = normalize(cameraPos - v.worldPos);
    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedoTex.rgb, metallic);
    
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
            LDir = normalize(l.position - v.worldPos);
            float dist = length(l.position - v.worldPos);
            attenuation = 1.0 / (dist * dist + 0.0001);
        }
        else if (l.type == 2)
        { // Spot
            LDir = normalize(l.position - v.worldPos);
            float dist = length(l.position - v.worldPos);
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
        Lo += (kD * albedoTex.rgb / PI + specular) * radiance * NdotL;
    }

    float3 ambient = float3(0.1, 0.1, 0.1) * albedoTex.rgb;
    float3 color = ambient + Lo;

    color = color / (color + float3(1.0, 1.0, 1.0));
    color = pow(color, float3(1.0 / 2.2, 1.0 / 2.2, 1.0 / 2.2));

    return float4(color, albedoTex.a);
}
