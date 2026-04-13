cbuffer Constants : register(b0)
{
    uint width;
    uint height;
    uint stepSize;
    uint passIndex;
    uint isLastPass;
};

RWTexture2D<float4> OutputDiffuse : register(u0);
RWTexture2D<float4> OutputSpecular : register(u1);

Texture2D<float4> InputDiffuse : register(t0);
Texture2D<float4> InputSpecular : register(t1);
Texture2D<float4> NormalMap : register(t2);
Texture2D<float4> WorldPosMap : register(t3);
Texture2D<float4> AlbedoMap : register(t4);
Texture2D<float2> VarianceMap : register(t5);

// 提取亮度的常數向量
static const float3 LUMINANCE_VECTOR = float3(0.2126f, 0.7152f, 0.0722f);

[numthreads(8, 8, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= width || DTid.y >= height)
        return;

    float3 centerNormal = NormalMap[DTid.xy].xyz;
    if (length(centerNormal) < 0.1f)
    {
        // 天空背景不處理，直接輸出
        OutputDiffuse[DTid.xy] = InputDiffuse[DTid.xy];
        OutputSpecular[DTid.xy] = InputSpecular[DTid.xy];
        return;
    }

    float centerRoughness = max(NormalMap[DTid.xy].w, 0.01f);
    float3 centerPos = WorldPosMap[DTid.xy].xyz;
    float3 centerAlbedo = max(AlbedoMap[DTid.xy].rgb, 0.001f);

    // 取得中心點顏色 (確保持續處於 Demodulated 狀態，也就是純光照 Irradiance)
    float4 centerDiff = InputDiffuse[DTid.xy];
    if (passIndex == 0)
        centerDiff.rgb /= centerAlbedo;
    float4 centerSpec = InputSpecular[DTid.xy];

    // ★ 核心 1：計算「純光照」的亮度，而非 Albedo 亮度
    float centerLumaDiff = dot(centerDiff.rgb, LUMINANCE_VECTOR);
    float centerLumaSpec = dot(centerSpec.rgb, LUMINANCE_VECTOR);

    // ★ 核心 2：方差轉標準差 (Standard Deviation)
    // 方差越大 (噪點越多)，標準差越大，後續的模糊權重就會越寬鬆，強力抹平噪點
    float2 centerVar = VarianceMap[DTid.xy].xy;
    float stdDevDiff = sqrt(max(centerVar.x, 0.0001f));
    float stdDevSpec = sqrt(max(centerVar.y, 0.0001f));

    float4 sumDiffuse = 0.0f;
    float sumWeightDiffuse = 0.0f;
    float4 sumSpecular = 0.0f;
    float sumWeightSpecular = 0.0f;

    // À-Trous 小波核 3x3 展開 (使用 5x5 會更好，但 3x3 效能較高)
    const int radius = 1;

    for (int y = -radius; y <= radius; ++y)
    {
        for (int x = -radius; x <= radius; ++x)
        {
            int2 offset = int2(x, y) * stepSize;
            int2 samplePos = clamp(int2(DTid.xy) + offset, int2(0, 0), int2(width - 1, height - 1));

            float4 sampleDiff = InputDiffuse[samplePos];
            if (passIndex == 0)
                sampleDiff.rgb /= max(AlbedoMap[samplePos].rgb, 0.001f);
            float4 sampleSpec = InputSpecular[samplePos];

            float3 sampleNormal = NormalMap[samplePos].xyz;
            float3 samplePosWorld = WorldPosMap[samplePos].xyz;

            // 1. 空間權重 (Gaussian-like)
            float spatialWeight = exp(-(x * x + y * y) / 2.0f);

            // 2. 法線權重 (★ 根據 stepSize 放寬限制，避免在大 Pass 時產生馬賽克)
            float normalPower = clamp(128.0f / (float) (stepSize * stepSize), 2.0f, 128.0f);
            float normalWeight = pow(max(0.0f, dot(centerNormal, sampleNormal)), normalPower);

            // 3. 平面深度權重 (★ 根據 stepSize 放寬，容忍曲面誤差)
            float planeDist = abs(dot(centerNormal, centerPos - samplePosWorld));
            float posWeight = exp(-planeDist / (0.01f * stepSize + 0.001f));

            // ==========================================
            // 4. Diffuse 動態亮度權重 (SVGF 的靈魂)
            // ==========================================
            float sampleLumaDiff = dot(sampleDiff.rgb, LUMINANCE_VECTOR);
            // 差距 / (標準差 * 敏感度)。標準差越大，指數越趨近 0，exp(0) = 1 (強制模糊！)
            float lumaWeightDiff = exp(-abs(centerLumaDiff - sampleLumaDiff) / (stdDevDiff * 4.0f + 0.001f));
            
            float diffW = spatialWeight * normalWeight * posWeight * lumaWeightDiff;
            sumDiffuse += sampleDiff * diffW;
            sumWeightDiffuse += diffW;

            // ==========================================
            // 5. Specular 權重 (受 Roughness 與 Specular 方差引導)
            // ==========================================
            float sampleLumaSpec = dot(sampleSpec.rgb, LUMINANCE_VECTOR);
            float lumaWeightSpec = exp(-abs(centerLumaSpec - sampleLumaSpec) / (stdDevSpec * 4.0f + 0.001f));
            
            // 粗糙度越低，Specular 越像鏡面，空間擴散必須越小以保留清晰反射
            float roughnessWeight = exp(-(x * x + y * y) / max(centerRoughness * centerRoughness * 2.0f, 0.05f));
            
            // 額外防護機制：
            // 如果表面極度光滑 (Roughness < 0.1f)，它應該表現得像鏡子。
            // 這時我們強制削弱所有非中心點 (x!=0 或 y!=0) 的權重貢獻，避免周圍像素污染高光反射。
            if (centerRoughness < 0.1f && (x != 0 || y != 0))
            {
                roughnessWeight *= 0.05f; // 強力截斷
            }
            
            float specW = spatialWeight * normalWeight * posWeight * lumaWeightSpec * roughnessWeight;
            sumSpecular += sampleSpec * specW;
            sumWeightSpecular += specW;
        }
    }

    float4 finalDiffuse = sumDiffuse / max(sumWeightDiffuse, 0.0001f);
    float4 finalSpecular = sumSpecular / max(sumWeightSpecular, 0.0001f);

    if (isLastPass == 1)
    {
        // 恢復 Albedo (Remodulation)
        finalDiffuse.rgb *= centerAlbedo;
        float3 combined = finalDiffuse.rgb + finalSpecular.rgb;

        // 保留您原本的 ToneMapping 與 Gamma 校正
        combined = combined / (combined + float3(1.0f, 1.0f, 1.0f));
        combined = pow(combined, float3(1.0f / 2.2f, 1.0f / 2.2f, 1.0f / 2.2f));

        OutputDiffuse[DTid.xy] = float4(combined, 1.0f);
        OutputSpecular[DTid.xy] = finalSpecular;
    }
    else
    {
        OutputDiffuse[DTid.xy] = finalDiffuse;
        OutputSpecular[DTid.xy] = finalSpecular;
    }
}