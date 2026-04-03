static const float PI = 3.14159265359;

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
    output.pos = mul(float4(input.pos, 1.0f), mvp);
    output.worldPos = mul(float4(input.pos, 1.0f), modelMatrix).xyz;
    output.normal = normalize(mul(input.normal, (float3x3) normalMatrix));
    output.uv = input.uv;
    return output;
}



Texture2D g_texture : register(t0);
Texture2D g_metallicRoughness : register(t1);
SamplerState g_sampler : register(s0);

float DistributionGGX(float3 N, float3 H, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    return a2 / max(PI * denom * denom, 0.0000001);
}

float GeometrySchlickGGX(float NdotV, float roughness)
{
    float r = (roughness + 1.0);
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float GeometrySmith(float3 N, float3 V, float3 L, float roughness)
{
    return GeometrySchlickGGX(max(dot(N, V), 0.0), roughness)
         * GeometrySchlickGGX(max(dot(N, L), 0.0), roughness);
}

float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

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
    float3 L = normalize(-lightDir);
    float3 H = normalize(V + L);

    float NdotL = max(dot(N, L), 0.0);

    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedoTex.rgb, metallic);

    float NDF = DistributionGGX(N, H, roughness);
    float G = GeometrySmith(N, V, L, roughness);
    float3 F = FresnelSchlick(max(dot(H, V), 0.0), F0);

    float3 numerator = NDF * G * F;
    float denominator = 4.0 * max(dot(N, V), 0.0) * NdotL;
    float3 specular = numerator / max(denominator, 0.001);

    float3 kD = (float3(1.0, 1.0, 1.0) - F) * (1.0 - metallic);

    float3 radiance = float3(3.0, 3.0, 3.0);
    float3 Lo = (kD * albedoTex.rgb / PI + specular) * radiance * NdotL;

    float3 ambient = float3(0.1, 0.1, 0.1) * albedoTex.rgb;
    float3 color = ambient + Lo;

    color = color / (color + float3(1.0, 1.0, 1.0));
    color = pow(color, float3(1.0 / 2.2, 1.0 / 2.2, 1.0 / 2.2));

    return float4(color, albedoTex.a);
}
