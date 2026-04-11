cbuffer Constants : register(b0)
{
    uint width;
    uint height;
};

// --- Outputs ---
RWTexture2D<float4> OutputGI : register(u0);
RWTexture2D<float4> OutputNormal : register(u1);
RWTexture2D<float4> OutputPos : register(u2);

// --- Inputs ---
Texture2D<float4> RawGI : register(t0);
Texture2D<float2> VelocityMap : register(t1);
Texture2D<float4> HistoryGI : register(t2);
Texture2D<float4> CurrentNormal : register(t3);
Texture2D<float4> CurrentPos : register(t4);
Texture2D<float4> HistoryNormal : register(t5);
Texture2D<float4> HistoryPos : register(t6);

SamplerState LinearSampler : register(s0);

// 色彩空間轉換函式
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

[numthreads(8, 8, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= width || DTid.y >= height)
        return;

    float2 uv = (float2(DTid.xy) + 0.5f) / float2(width, height);
    float4 currentColor = RawGI[DTid.xy];
    float2 velocity = VelocityMap[DTid.xy];
    
    float3 centerNormal = CurrentNormal[DTid.xy].xyz;
    float3 centerPos = CurrentPos[DTid.xy].xyz;

    // 將當下畫面的法線與座標寫入 History，供下一幀對比使用
    OutputNormal[DTid.xy] = float4(centerNormal, 1.0f);
    OutputPos[DTid.xy] = float4(centerPos, 1.0f);

    float2 prevUV = uv - velocity;
    float4 historyColor = currentColor;
    
    // 預設為遮擋/無效狀態
    float historyValid = 0.0f;
    
    if (all(prevUV >= 0.0f) && all(prevUV <= 1.0f))
    {
        float3 histNormal = HistoryNormal.SampleLevel(LinearSampler, prevUV, 0).xyz;
        float3 histPos = HistoryPos.SampleLevel(LinearSampler, prevUV, 0).xyz;
        
        // SVGF 遮擋剔除 (History Rejection)
        // 法線角度差異 (越接近 1 代表角度越一致)
        float normalDist = dot(centerNormal, histNormal);
        // 投影平面距離 (將位移誤差投影到法線上，用以容忍攝影機在表面上平移滑動)
        float planeDist = abs(dot(centerNormal, centerPos - histPos));
        
        // 若法線差異過大或偏離表面過遠，則判定為不同物體 (觸發剔除)
        if (normalDist > 0.8f && planeDist < 0.1f)
        {
            historyValid = 1.0f; // 歷史資料有效，進入處理階段
            historyColor = HistoryGI.SampleLevel(LinearSampler, prevUV, 0);

            // YCoCg 空間 Clamping
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

            float3 colorMin = mu - 1.25f * sigma;
            float3 colorMax = mu + 1.25f * sigma;

            float3 histYCoCg = RGBToYCoCg(historyColor.rgb);
            histYCoCg = clamp(histYCoCg, colorMin, colorMax);
            historyColor = float4(YCoCgToRGB(histYCoCg), historyColor.a);
        }
    }

    float velMag = length(velocity);
    float baseBlend = lerp(0.06f, 0.25f, saturate(velMag * 100.0f));
    
    // 如果 historyValid 為 0 (被遮擋/新出現)，強制使用 1.0 的權重 (100% 當前畫面)，瞬間切斷所有殘影！
    float blendWeight = lerp(1.0f, baseBlend, historyValid);
    
    float4 finalColor = lerp(historyColor, currentColor, blendWeight);
    OutputGI[DTid.xy] = finalColor;
}