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

Texture2D g_baseColorMap : register(t0);
Texture2D g_mrMap : register(t1);
SamplerState g_sampler : register(s0);

struct PSOutput
{
    float4 AlbedoRoughness : SV_Target0;
    float4 NormalMetallic : SV_Target1;
    float4 WorldPos : SV_Target2;
};

PSOutput PSMain(VSOutput input)
{
    PSOutput output;
    
    float4 albedo = g_baseColorMap.Sample(g_sampler, input.uv);
    clip(albedo.a - 0.5f); // 簡單的 Alpha Test
    
    // 假設 MR 貼圖 G通道=Roughness, B通道=Metallic
    float4 mr = g_mrMap.Sample(g_sampler, input.uv);
    
    output.AlbedoRoughness = float4(albedo.rgb, mr.g);
    output.NormalMetallic = float4(normalize(input.normal), mr.b);
    output.WorldPos = float4(input.worldPos, 1.0f);
    
    return output;
}