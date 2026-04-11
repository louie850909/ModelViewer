cbuffer PassConstants : register(b0)
{
    matrix viewProj;
    matrix unjitteredViewProj;
    matrix prevUnjitteredViewProj;
};

cbuffer ObjectConstants : register(b1)
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
    float4 clipPos : POSITION1;
    float4 prevClipPos : POSITION2;
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    // 實際的頂點位置使用有 Jitter 的矩陣
    float4 worldPos = mul(float4(input.pos, 1.0f), modelMatrix);
    output.worldPos = worldPos.xyz;
    output.pos = mul(worldPos, viewProj);
    
    // 透過 Adjugate Matrix (伴隨矩陣) 動態推導法線的 Inverse 矩陣
    float3x3 m3x3 = (float3x3) modelMatrix;
    float3x3 adj = transpose(float3x3(
        cross(m3x3[1], m3x3[2]),
        cross(m3x3[2], m3x3[0]),
        cross(m3x3[0], m3x3[1])
    ));
    output.normal = normalize(mul(input.normal, adj));
    
    output.uv = input.uv;
    
    // 記錄當前幀與上一幀的 Clip Space 位置 (供 Velocity 運算), 必須使用【無 Jitter】的矩陣
    output.clipPos = mul(worldPos, unjitteredViewProj);
    output.prevClipPos = mul(worldPos, prevUnjitteredViewProj);
    
    return output;
}

Texture2D g_baseColorMap : register(t0);
Texture2D g_mrMap : register(t1);
SamplerState g_sampler : register(s0);

struct PSOutput
{
    float4 Albedo : SV_Target0;
    float4 NormalRoughness : SV_Target1;
    float4 WorldPosMetallic : SV_Target2;
    float2 Velocity : SV_Target3;
};

PSOutput PSMain(VSOutput input)
{
    PSOutput output;
    
    float4 albedo = g_baseColorMap.Sample(g_sampler, input.uv);
    clip(albedo.a - 0.5f); // 簡單的 Alpha Test
    
    // 假設 MR 貼圖 G通道=Roughness, B通道=Metallic
    float4 mr = g_mrMap.Sample(g_sampler, input.uv);
    
    output.Albedo = float4(albedo.rgba);
    output.NormalRoughness = float4(normalize(input.normal), mr.g);
    output.WorldPosMetallic = float4(input.worldPos, mr.b);
    
    // 計算螢幕空間動態向量 (Velocity)
    float2 ndc = input.clipPos.xy / input.clipPos.w;
    float2 prevNdc = input.prevClipPos.xy / input.prevClipPos.w;
    
    // NDC [-1, 1] 轉 UV [0, 1] 空間 (注意 Y 軸反轉)
    output.Velocity = (ndc - prevNdc) * float2(0.5f, -0.5f);
    
    return output;
}