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

[numthreads(8, 8, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= width || DTid.y >= height)
        return;

    float3 centerNormal = NormalMap[DTid.xy].xyz;
    float centerRoughness = max(NormalMap[DTid.xy].w, 0.01f);
    float3 centerPos = WorldPosMap[DTid.xy].xyz;
    float3 centerAlbedo = max(AlbedoMap[DTid.xy].rgb, 0.001f);

    // ★ 新增：計算中心像素的 Albedo Luma，用於邊界保護
    float centerLuma = dot(centerAlbedo, float3(0.2126f, 0.7152f, 0.0722f));

    if (length(centerNormal) < 0.1f)
    {
        OutputDiffuse[DTid.xy] = InputDiffuse[DTid.xy];
        OutputSpecular[DTid.xy] = InputSpecular[DTid.xy];
        return;
    }

    float4 sumDiffuse = 0.0f;
    float sumWeightDiffuse = 0.0f;
    float4 sumSpecular = 0.0f;
    float sumWeightSpecular = 0.0f;
    
    float2 centerVar = VarianceMap[DTid.xy].xy;
    float varScaleDiff = 1.0f + centerVar.x * 20.0f; // Variance 越高，Scale 越大
    float varScaleSpec = 1.0f + centerVar.y * 30.0f;

    const int radius = 2;

    for (int y = -radius; y <= radius; ++y)
    {
        for (int x = -radius; x <= radius; ++x)
        {
            int2 offset = int2(x, y) * stepSize;
            int2 samplePos = clamp(int2(DTid.xy) + offset,
                                   int2(0, 0), int2(width - 1, height - 1));

            float4 sampleDiff = InputDiffuse[samplePos];
            float4 sampleSpec = InputSpecular[samplePos];

            if (passIndex == 0)
            {
                sampleDiff.rgb /= max(AlbedoMap[samplePos].rgb, 0.001f);
            }

            float3 sampleNormal = NormalMap[samplePos].xyz;
            float3 samplePosWorld = WorldPosMap[samplePos].xyz;

            float spatialWeight = exp(-(x * x + y * y) / 4.5f);

            float normalWeight = pow(max(0.0f, dot(centerNormal, sampleNormal)), 64.0f);

            float planeDist = abs(dot(centerNormal, centerPos - samplePosWorld));
            float posWeight = exp(-planeDist * 10.0f);

            // ★ 新增：Albedo Luma 邊界保護
            // Luma 差距大 → 跨越紋理邊界 → 大幅降低 Diffuse 模糊權重
            float sampleLuma = dot(max(AlbedoMap[samplePos].rgb, 0.001f),
                                   float3(0.2126f, 0.7152f, 0.0722f));
            
            // 將 varScaleDiff / varScaleSpec 作為分母。
            // Variance 高時 -> 指數部分趨近 0 -> exp(0) = 1 (模糊放寬)
            float lumaWeight = exp(-abs(centerLuma - sampleLuma) * 30.0f / varScaleDiff);
            float diffW = spatialWeight * normalWeight * posWeight * lumaWeight;

            // Specular 的權重獨立使用 Spec Variance 引導
            float roughnessWeight = exp(-(x * x + y * y) / max(centerRoughness * centerRoughness * 10.0f * varScaleSpec, 0.10f));
            float specW = diffW * roughnessWeight;

            sumDiffuse += sampleDiff * diffW;
            sumWeightDiffuse += diffW;

            sumSpecular += sampleSpec * specW;
            sumWeightSpecular += specW;
        }
    }

    float4 finalDiffuse = sumDiffuse / max(sumWeightDiffuse, 0.0001f);
    float4 finalSpecular = sumSpecular / max(sumWeightSpecular, 0.0001f);

    if (isLastPass == 1)
    {
        finalDiffuse.rgb *= centerAlbedo;
        float3 combined = finalDiffuse.rgb + finalSpecular.rgb;

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