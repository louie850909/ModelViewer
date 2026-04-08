cbuffer Constants : register(b0)
{
    uint width;
    uint height;
};

float3 RGBToYCoCg(float3 rgb)
{
    return float3(
        rgb.r * 0.25f + rgb.g * 0.5f + rgb.b * 0.25f,
        rgb.r * 0.5f - rgb.b * 0.5f,
        -rgb.r * 0.25f + rgb.g * 0.5f - rgb.b * 0.25f
    );
}

float3 YCoCgToRGB(float3 ycocg)
{
    return float3(
        ycocg.x + ycocg.y - ycocg.z,
        ycocg.x + ycocg.z,
        ycocg.x - ycocg.y - ycocg.z
    );
}

RWTexture2D<float4> OutputGI : register(u0);
Texture2D<float4> RawGI : register(t0);
Texture2D<float2> VelocityMap : register(t1);
Texture2D<float4> HistoryGI : register(t2);

SamplerState LinearSampler : register(s0);

[numthreads(8, 8, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= width || DTid.y >= height)
        return;

    float2 uv = (float2(DTid.xy) + 0.5f) / float2(width, height);
    float4 currentColor = RawGI[DTid.xy];
    float2 velocity = VelocityMap[DTid.xy];

    // 利用 Velocity 推算這個像素在上一幀的 UV
    float2 prevUV = uv - velocity;
    float4 historyColor = currentColor;
    
    // 檢查上一幀座標是否還在螢幕範圍內
    if (all(prevUV >= 0.0f) && all(prevUV <= 1.0f))
    {
        historyColor = HistoryGI.SampleLevel(LinearSampler, prevUV, 0);

        // Neighborhood Clamping: 利用當前畫面的 3x3 區域來限制歷史顏色，防止 Ghosting
        float3 m1 = 0.0f;
        float3 m2 = 0.0f;
        for (int y = -1; y <= 1; y++)
        {
            for (int x = -1; x <= 1; x++)
            {
                int2 samplePos = clamp(int2(DTid.xy) + int2(x, y), int2(0, 0), int2(width - 1, height - 1));
                float3 c = RGBToYCoCg(RawGI[samplePos].rgb);
                m1 += c;
                m2 += c * c;
            }
        }
        float3 mu = m1 / 9.0f;
        float3 sigma = sqrt(max(m2 / 9.0f - mu * mu, 0.0f));
        
        float boxMultiplier = 1.25f;
        float3 colorMin = mu - (boxMultiplier * sigma + 0.005f);
        float3 colorMax = mu + (boxMultiplier * sigma + 0.005f);

        // 將歷史顏色轉入 YCoCg 進行裁切，再轉回 RGB
        float3 histYCoCg = RGBToYCoCg(historyColor.rgb);
        histYCoCg = clamp(histYCoCg, colorMin, colorMax);
        historyColor = float4(YCoCgToRGB(histYCoCg), historyColor.a);
    }
    else
    {
        // 如果找不到歷史座標，只能使用當前畫面
        historyColor = currentColor;
    }
    float velMag = length(velocity);

    // 動態權重：如果像素移動速度越快，我們就越不信任歷史畫面 (提高新畫面的權重)，藉此消除殘影
    float blendWeight = lerp(0.08f, 0.2f, saturate(velMag * 100.0f));
    float4 finalColor = lerp(historyColor, currentColor, blendWeight);

    OutputGI[DTid.xy] = finalColor;
}