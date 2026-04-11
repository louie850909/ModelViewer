cbuffer Constants : register(b0)
{
    uint width;
    uint height;
};

// --- Outputs (4 個) ---
RWTexture2D<float4> OutputDiffuse : register(u0);
RWTexture2D<float4> OutputSpecular : register(u1);
RWTexture2D<float4> OutputNormal : register(u2);
RWTexture2D<float4> OutputPos : register(u3);

// --- Inputs (9 個) ---
Texture2D<float4> RawDiffuse : register(t0);
Texture2D<float4> RawSpecular : register(t1);
Texture2D<float2> VelocityMap : register(t2);
Texture2D<float4> HistoryDiffuse : register(t3);
Texture2D<float4> HistorySpecular : register(t4);
Texture2D<float4> CurrentNormal : register(t5);
Texture2D<float4> CurrentPos : register(t6);
Texture2D<float4> HistoryNormal : register(t7);
Texture2D<float4> HistoryPos : register(t8);

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
    float2 velocity = VelocityMap[DTid.xy];
    
    float4 currDiffuse = RawDiffuse[DTid.xy];
    float4 currSpecular = RawSpecular[DTid.xy];
    float3 centerNormal = CurrentNormal[DTid.xy].xyz;
    float3 centerPos = CurrentPos[DTid.xy].xyz;

    // 將當下畫面的法線與座標寫入 History，供下一幀對比使用
    OutputNormal[DTid.xy] = float4(centerNormal, 1.0f);
    OutputPos[DTid.xy] = float4(centerPos, 1.0f);

    float2 prevUV = uv - velocity;
    float4 histDiffuse = currDiffuse;
    float4 histSpecular = currSpecular;
    float historyValid = 0.0f;
    
    if (all(prevUV >= 0.0f) && all(prevUV <= 1.0f))
    {
        float3 hNormal = HistoryNormal.SampleLevel(LinearSampler, prevUV, 0).xyz;
        float3 hPos = HistoryPos.SampleLevel(LinearSampler, prevUV, 0).xyz;
        
        float normalDist = dot(centerNormal, hNormal);
        float planeDist = abs(dot(centerNormal, centerPos - hPos));
        
        // SVGF 遮擋剔除
        if (normalDist > 0.8f && planeDist < 0.1f)
        {
            historyValid = 1.0f;
            histDiffuse = HistoryDiffuse.SampleLevel(LinearSampler, prevUV, 0);
            histSpecular = HistorySpecular.SampleLevel(LinearSampler, prevUV, 0);

            // 同時計算 Diffuse 與 Specular 的 YCoCg 變異數
            float3 m1_diff = 0.0f, m2_diff = 0.0f;
            float3 m1_spec = 0.0f, m2_spec = 0.0f;
            
            for (int y = -1; y <= 1; y++)
            {
                for (int x = -1; x <= 1; x++)
                {
                    int2 samplePos = clamp(int2(DTid.xy) + int2(x, y), int2(0, 0), int2(width - 1, height - 1));
                    
                    float3 c_diff = RGBToYCoCg(RawDiffuse[samplePos].rgb);
                    m1_diff += c_diff;
                    m2_diff += c_diff * c_diff;
                    
                    float3 c_spec = RGBToYCoCg(RawSpecular[samplePos].rgb);
                    m1_spec += c_spec;
                    m2_spec += c_spec * c_spec;
                }
            }
            
            // Diffuse 裁切
            float3 mu_diff = m1_diff / 9.0f;
            float3 sigma_diff = sqrt(max(m2_diff / 9.0f - mu_diff * mu_diff, 0.0f));
            float3 histY_diff = RGBToYCoCg(histDiffuse.rgb);
            histY_diff = clamp(histY_diff, mu_diff - 1.25f * sigma_diff, mu_diff + 1.25f * sigma_diff);
            histDiffuse = float4(YCoCgToRGB(histY_diff), histDiffuse.a);

            // Specular 裁切
            float3 mu_spec = m1_spec / 9.0f;
            float3 sigma_spec = sqrt(max(m2_spec / 9.0f - mu_spec * mu_spec, 0.0f));
            float3 histY_spec = RGBToYCoCg(histSpecular.rgb);
            histY_spec = clamp(histY_spec, mu_spec - 1.25f * sigma_spec, mu_spec + 1.25f * sigma_spec);
            histSpecular = float4(YCoCgToRGB(histY_spec), histSpecular.a);
        }
    }

    float velMag = length(velocity);
    float baseBlend = lerp(0.06f, 0.25f, saturate(velMag * 100.0f));
    float blendWeight = lerp(1.0f, baseBlend, historyValid);
    
    OutputDiffuse[DTid.xy] = lerp(histDiffuse, currDiffuse, blendWeight);
    OutputSpecular[DTid.xy] = lerp(histSpecular, currSpecular, blendWeight);
}