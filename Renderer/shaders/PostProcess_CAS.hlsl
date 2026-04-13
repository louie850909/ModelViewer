Texture2D<float4> InputColor : register(t0);
RWTexture2D<float4> OutputColor : register(u0);

cbuffer CASConstants : register(b0)
{
    float Sharpness; // 銳化強度，建議範圍 0.0 (無) 到 1.0 (最強)
    int Width;
    int Height;
    float padding;
};

// 亮度計算
float Min3(float a, float b, float c)
{
    return min(a, min(b, c));
}
float Max3(float a, float b, float c)
{
    return max(a, max(b, c));
}

[numthreads(8, 8, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID)
{
    int2 pos = int2(DTid.xy);
    if (pos.x >= Width || pos.y >= Height)
        return;

    // 採樣十字型鄰居
    //   b
    // d e f
    //   h
    float3 a = InputColor[clamp(pos + int2(0, -1), 0, int2(Width - 1, Height - 1))].rgb; // b
    float3 b = InputColor[clamp(pos + int2(-1, 0), 0, int2(Width - 1, Height - 1))].rgb; // d
    float3 c = InputColor[pos].rgb; // e (中心點)
    float3 d = InputColor[clamp(pos + int2(1, 0), 0, int2(Width - 1, Height - 1))].rgb; // f
    float3 e = InputColor[clamp(pos + int2(0, 1), 0, int2(Width - 1, Height - 1))].rgb; // h

    // 計算局部區域的最小與最大 RGB 值
    float3 minRGB = Min3(Min3(a, b, c), d, e);
    float3 maxRGB = Max3(Max3(a, b, c), d, e);

    // 確保不會除以零
    maxRGB = max(maxRGB, 0.00001f);

    // 計算權重：局部對比度越高，銳化權重越低，避免邊緣出現過度銳化的光暈 (Ringing)
    float3 ampLimit = min(minRGB, 1.0f - maxRGB);
    float3 w = ampLimit / maxRGB;
    
    // 控制最終銳化強度
    // Sharpness 從 0(不銳化) 到 1(最大銳化)。公式轉換為 CAS 內部參數：
    float CAS_Param = lerp(-0.125f, -0.2f, clamp(Sharpness, 0.0f, 1.0f));
    w *= CAS_Param;

    // 應用濾波： center + neighbors * weight
    float3 outputRGB = (a * w + b * w + d * w + e * w + c) / (1.0f + 4.0f * w);

    // 輸出，保留原本的 Alpha (如果有的話)
    OutputColor[pos] = float4(saturate(outputRGB), InputColor[pos].a);
}