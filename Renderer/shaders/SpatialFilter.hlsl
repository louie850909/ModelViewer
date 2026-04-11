cbuffer Constants : register(b0)
{
    uint width;
    uint height;
    uint stepSize;
    uint passIndex; // 當前是第幾個 Pass
    uint isLastPass; // 1 表示最後一個 Pass，0 則否
};

// 雙輸出
RWTexture2D<float4> OutputDiffuse : register(u0);
RWTexture2D<float4> OutputSpecular : register(u1);

// 雙輸入 + 幾何材質資訊
Texture2D<float4> InputDiffuse : register(t0);
Texture2D<float4> InputSpecular : register(t1);
Texture2D<float4> NormalMap : register(t2);
Texture2D<float4> WorldPosMap : register(t3);
Texture2D<float4> AlbedoMap : register(t4);

[numthreads(8, 8, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= width || DTid.y >= height)
        return;

    float3 centerNormal = NormalMap[DTid.xy].xyz;
    float centerRoughness = max(NormalMap[DTid.xy].w, 0.01f);
    float3 centerPos = WorldPosMap[DTid.xy].xyz;
    float3 centerAlbedo = max(AlbedoMap[DTid.xy].rgb, 0.001f);
    
    // 如果打到天空，略過降噪與解調直接輸出
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

    // 半徑為 2，在 À-Trous 中代表 5x5 的卷積矩陣
    const int radius = 2;
    
    for (int y = -radius; y <= radius; ++y)
    {
        for (int x = -radius; x <= radius; ++x)
        {
            // 將基礎座標偏移乘上 stepSize 實現範圍膨脹
            int2 offset = int2(x, y) * stepSize;
            int2 samplePos = clamp(int2(DTid.xy) + offset, int2(0, 0), int2(width - 1, height - 1));

            float4 sampleDiff = InputDiffuse[samplePos];
            float4 sampleSpec = InputSpecular[samplePos];
            
            // // 漫反射要除法解調 (Demodulation)
            // 如果是第一個 Pass，從輸入的帶紋理光照中「剔除」紋理，轉換為純淨光照
            if (passIndex == 0)
            {
                sampleDiff.rgb /= max(AlbedoMap[samplePos].rgb, 0.001f);
            }

            float3 sampleNormal = NormalMap[samplePos].xyz;
            float3 samplePosWorld = WorldPosMap[samplePos].xyz;

            // 權重計算保持不變
            float spatialWeight = exp(-(x * x + y * y) / 4.5f);

            // 法線權重 (法線差異過大代表轉角或不同面，停止模糊)
            float normalWeight = pow(max(0.0f, dot(centerNormal, sampleNormal)), 64.0f);

            // 深度/平面權重 (最強大的改進：將距離投影到法線上)
            // 這讓我們在同一個平坦表面上即使採樣跨度很大，平面距離也幾乎為 0，允許盡情模糊；
            // 但只要稍微跨出邊緣或遇到凹凸，權重就會瞬間暴跌至 0。
            float planeDist = abs(dot(centerNormal, centerPos - samplePosWorld));
            float posWeight = exp(-planeDist * 10.0f);

            float diffW = spatialWeight * normalWeight * posWeight;
            
            // 粗糙度引導 (Roughness-Guided) 權重
            // 若粗糙度趨近 0，分母極小，非中心的權重會瞬間跌落至 0 (保留銳利鏡面)
            // 若粗糙度越高，權重衰減越慢，允許大範圍模糊
            float roughnessWeight = exp(-(x * x + y * y) / max(centerRoughness * centerRoughness * 10.0f, 0.05f));
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
        // 最終合成：Diffuse 乘回 Albedo，並加上根據粗糙度模糊完的高光！
        finalDiffuse.rgb *= centerAlbedo;
        OutputDiffuse[DTid.xy] = float4(finalDiffuse.rgb + finalSpecular.rgb, 1.0f);
        OutputSpecular[DTid.xy] = finalSpecular; // 僅佔位用
    }
    else
    {
        OutputDiffuse[DTid.xy] = finalDiffuse;
        OutputSpecular[DTid.xy] = finalSpecular;
    }
}