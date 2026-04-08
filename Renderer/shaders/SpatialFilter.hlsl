cbuffer Constants : register(b0)
{
    uint width;
    uint height;
};

RWTexture2D<float4> OutputGI : register(u0);
Texture2D<float4> InputGI : register(t0);
Texture2D<float4> NormalMap : register(t1);
Texture2D<float4> WorldPosMap : register(t2);

[numthreads(8, 8, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= width || DTid.y >= height)
        return;

    float3 centerNormal = NormalMap[DTid.xy].xyz;
    float3 centerPos = WorldPosMap[DTid.xy].xyz;
    
    // 如果打到天空 (法線長度為 0)，直接輸出原始顏色
    if (length(centerNormal) < 0.1f)
    {
        OutputGI[DTid.xy] = InputGI[DTid.xy];
        return;
    }

    float4 sumColor = 0.0f;
    float sumWeight = 0.0f;

    const int radius = 2; // 5x5 的採樣區域
    
    for (int y = -radius; y <= radius; ++y)
    {
        for (int x = -radius; x <= radius; ++x)
        {
            int2 samplePos = clamp(int2(DTid.xy) + int2(x, y), int2(0, 0), int2(width - 1, height - 1));
            
            float4 sampleColor = InputGI[samplePos];
            float3 sampleNormal = NormalMap[samplePos].xyz;
            float3 samplePosWorld = WorldPosMap[samplePos].xyz;

            // 1. 空間權重 (越遠權重越低)
            float spatialWeight = exp(-(x * x + y * y) / 4.5f);
            
            // 2. 法線權重 (法線差異過大代表轉角或不同面，停止模糊)
            float normalWeight = pow(max(0.0f, dot(centerNormal, sampleNormal)), 64.0f);
            
            // 3. 深度/位置權重 (世界座標距離太遠代表不同物體邊緣，停止模糊)
            float posWeight = exp(-length(centerPos - samplePosWorld) * 5.0f);

            float w = spatialWeight * normalWeight * posWeight;
            
            sumColor += sampleColor * w;
            sumWeight += w;
        }
    }

    OutputGI[DTid.xy] = sumColor / max(sumWeight, 0.0001f);
}