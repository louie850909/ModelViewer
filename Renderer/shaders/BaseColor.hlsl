// ---- Constant Buffer ----
cbuffer SceneConstants : register(b0)
{
    float4x4 mvp;       // Model * View * Projection
    float4x4 normalMatrix;
    float3 lightDir;    // World space, normalized
    float _pad;
    float4 baseColor;   // RGBA
};

// ---- Vertex Shader ----
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
};

VSOut VSMain(VSIn v)
{
    VSOut o;
    o.pos = mul(float4(v.pos, 1.0), mvp);
    o.normal = v.normal;
    o.uv = v.uv;
    return o;
}

Texture2D       g_texture : register(t0); // ← 貼圖資源 (Texture 0)
SamplerState    g_sampler : register(s0); // ← 採樣器 (Sampler 0)

// ---- Pixel Shader ----
float4 PSMain(VSOut v) : SV_TARGET
{
    // 1. 採樣貼圖
    float4 texColor = g_texture.Sample(g_sampler, v.uv);
    // 2. 計算世界空間法線
    float3 worldNormal = normalize(mul((float3x3) normalMatrix, v.normal));
    
    // 3. 簡單漫反射光照
    float ndotl = saturate(dot(worldNormal, -lightDir));
    float ambient = 0.2f;
    
    // 4. 混合輸出 (避免純黑)
    float3 finalColor = texColor.rgb * (ndotl + ambient);
    
    return float4(finalColor, texColor.a); // 忽略貼圖
}