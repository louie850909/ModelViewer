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
Texture2D g_normalMap : register(t2);
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
    
    // 法線貼圖與動態切線空間 (TBN) 計算
    float3 localNormalMap = g_normalMap.Sample(g_sampler, input.uv).xyz;
    localNormalMap = localNormalMap * 2.0f - 1.0f; // 轉換到 [-1, 1]
    
    // 修正 glTF (OpenGL) 與 DX12 的 +Y 軸向差異
    localNormalMap.y = -localNormalMap.y;

    float3 worldNormal = normalize(input.normal);
    
    // 使用 Pixel Shader 專屬的 ddx/ddy 計算切線空間
    float3 dp1 = ddx(input.worldPos);
    float3 dp2 = ddy(input.worldPos);
    float2 duv1 = ddx(input.uv);
    float2 duv2 = ddy(input.uv);

    float det = duv1.x * duv2.y - duv1.y * duv2.x;
    float3 worldTangent;
    
    // 【防呆機制】避免 UV 退化導致除以零 (NaN 爆炸)
    if (abs(det) < 1e-6f)
    {
        float3 up = abs(worldNormal.z) < 0.999f ? float3(0, 0, 1) : float3(1, 0, 0);
        worldTangent = normalize(cross(up, worldNormal));
    }
    else
    {
        float r = 1.0f / det;
        worldTangent = normalize((dp1 * duv2.y - dp2 * duv1.y) * r);
    }

    // Gram-Schmidt 正交化，確保切線絕對垂直於法線
    worldTangent = normalize(worldTangent - dot(worldTangent, worldNormal) * worldNormal);
    
    // 根據 UV 面積正負號修正鏡像 UV 的副切線方向
    float handedness = (det < 0.0f) ? -1.0f : 1.0f;
    float3 worldBitangent = cross(worldNormal, worldTangent) * handedness;
    
    // 建立 TBN 矩陣並轉換法線
    float3x3 tbn = float3x3(worldTangent, worldBitangent, worldNormal);
    float3 finalNormal = normalize(mul(localNormalMap, tbn));
    
    output.Albedo = float4(albedo.rgba);
    output.NormalRoughness = float4(finalNormal, mr.g);
    output.WorldPosMetallic = float4(input.worldPos, mr.b);
    
    // 計算螢幕空間動態向量 (Velocity)
    float2 ndc = input.clipPos.xy / input.clipPos.w;
    float2 prevNdc = input.prevClipPos.xy / input.prevClipPos.w;
    
    // NDC [-1, 1] 轉 UV [0, 1] 空間 (注意 Y 軸反轉)
    output.Velocity = (ndc - prevNdc) * float2(0.5f, -0.5f);
    
    return output;
}