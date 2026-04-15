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

cbuffer MaterialFactors : register(b2)
{
    float roughnessFactor;
    float metallicFactor;
    float isTransmission;
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
    // 実際の頂点位置には Jitter ありの行列を使用
    float4 worldPos = mul(float4(input.pos, 1.0f), modelMatrix);
    output.worldPos = worldPos.xyz;
    output.pos = mul(worldPos, viewProj);
    
    // Adjugate Matrix (余因子行列) を使って法線の逆行列を動的に導出
    float3x3 m3x3 = (float3x3) modelMatrix;
    float3x3 adj = transpose(float3x3(
        cross(m3x3[1], m3x3[2]),
        cross(m3x3[2], m3x3[0]),
        cross(m3x3[0], m3x3[1])
    ));
    output.normal = normalize(mul(input.normal, adj));
    
    output.uv = input.uv;
    
    // 現在フレームと前フレームの Clip Space 位置を記録 (Velocity 計算用)、【Jitter なし】の行列を使用する必要がある
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
    clip(albedo.a - 0.5f); // シンプルな Alpha Test
    
    // MR テクスチャは G チャンネル=Roughness、B チャンネル=Metallic と仮定
    float4 mr = g_mrMap.Sample(g_sampler, input.uv);
    
    // 法線マップと動的接線空間 (TBN) の計算
    float3 localNormalMap = g_normalMap.Sample(g_sampler, input.uv).xyz;
    localNormalMap = localNormalMap * 2.0f - 1.0f; // [-1, 1] に変換

    // glTF (OpenGL) と DX12 の +Y 軸の差異を修正
    localNormalMap.y = -localNormalMap.y;

    float3 worldNormal = normalize(input.normal);
    
    // Pixel Shader 専用の ddx/ddy を使って接線空間を計算
    float3 dp1 = ddx(input.worldPos);
    float3 dp2 = ddy(input.worldPos);
    float2 duv1 = ddx(input.uv);
    float2 duv2 = ddy(input.uv);

    float det = duv1.x * duv2.y - duv1.y * duv2.x;
    float3 worldTangent;
    
    // 【フェイルセーフ】UV の縮退によるゼロ除算 (NaN 爆発) を防ぐ
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

    // Gram-Schmidt 正規直交化で接線が法線に完全に垂直であることを保証
    worldTangent = normalize(worldTangent - dot(worldTangent, worldNormal) * worldNormal);
    
    // UV 面積の符号に基づいてミラー UV の従接線方向を修正
    float handedness = (det < 0.0f) ? -1.0f : 1.0f;
    float3 worldBitangent = cross(worldNormal, worldTangent) * handedness;
    
    // TBN 行列を構築して法線を変換
    float3x3 tbn = float3x3(worldTangent, worldBitangent, worldNormal);
    float3 finalNormal = normalize(mul(localNormalMap, tbn));
    
    // 透過材質 (ガラス等) は denoiser の specular カーネルをタイトに保つため、
    // GBuffer の roughness を強制的に 0.02 に設定する。
    // これにより鏡面/屈折のサンプルが広域ブラーされず細部が保持される。
    float finalRoughness = saturate(mr.g * roughnessFactor);
    if (isTransmission > 0.5f)
        finalRoughness = 0.02f;

    float finalMetallic = saturate(mr.b * metallicFactor);

    output.Albedo = float4(albedo.rgba);
    output.NormalRoughness = float4(finalNormal, finalRoughness);
    output.WorldPosMetallic = float4(input.worldPos, finalMetallic);
    
    // スクリーン空間のモーションベクトル (Velocity) を計算
    float2 ndc = input.clipPos.xy / input.clipPos.w;
    float2 prevNdc = input.prevClipPos.xy / input.prevClipPos.w;
    
    // NDC [-1, 1] を UV [0, 1] 空間に変換 (Y 軸反転に注意)
    output.Velocity = (ndc - prevNdc) * float2(0.5f, -0.5f);
    
    return output;
}