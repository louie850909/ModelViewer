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
    float3 Lo = (kD * albedo.rgb / PI + specular) * radiance * NdotL;

    float3 ambient = float3(0.1, 0.1, 0.1) * albedo.rgb;
    float3 color = ambient + Lo;

    color = color / (color + float3(1.0, 1.0, 1.0));
    color = pow(color, float3(1.0 / 2.2, 1.0 / 2.2, 1.0 / 2.2));

    return float4(color, 1);
}