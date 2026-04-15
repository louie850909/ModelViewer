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

// 輝度抽出用の定数ベクトル
static const float3 LUMINANCE_VECTOR = float3(0.2126f, 0.7152f, 0.0722f);

[numthreads(8, 8, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= width || DTid.y >= height)
        return;

    float3 centerNormal = NormalMap[DTid.xy].xyz;
    if (length(centerNormal) < 0.1f)
    {
        // 空の背景は処理せず、そのまま出力
        OutputDiffuse[DTid.xy] = InputDiffuse[DTid.xy];
        OutputSpecular[DTid.xy] = InputSpecular[DTid.xy];
        return;
    }

    float centerRoughness = max(NormalMap[DTid.xy].w, 0.01f);
    float3 centerPos = WorldPosMap[DTid.xy].xyz;
    float3 centerAlbedo = max(AlbedoMap[DTid.xy].rgb, 0.001f);

    // 中心点の色を取得 (常に Demodulated 状態、つまり純粋な照明 Irradiance であることを保証)
    float4 centerDiff = InputDiffuse[DTid.xy];
    float4 centerSpec = InputSpecular[DTid.xy];
    if (passIndex == 0)
    {
        centerDiff.rgb /= centerAlbedo;
        // ==========================================
        // 鏡面ハイライト輝度安定化 (Tonemap Demodulation)
        // 極端な HDR ハイライト値を抑制し、空間ブラー時にハロが爆発しないようにする
        // ==========================================
        float specLuma = dot(centerSpec.rgb, LUMINANCE_VECTOR);
        centerSpec.rgb /= (1.0f + specLuma);
    }

    // ★ コア 1：Albedo 輝度ではなく「純粋な照明」の輝度を計算
    float centerLumaDiff = dot(centerDiff.rgb, LUMINANCE_VECTOR);
    float centerLumaSpec = dot(centerSpec.rgb, LUMINANCE_VECTOR);

    // ★ コア 2：分散を標準偏差 (Standard Deviation) に変換
    // 分散が大きいほど (ノイズが多いほど)、標準偏差が大きくなり、後続のブラー重みが緩くなってノイズを強力に平滑化
    float2 centerVar = VarianceMap[DTid.xy].xy;
    float stdDevDiff = sqrt(max(centerVar.x, 0.0001f));
    float stdDevSpec = sqrt(max(centerVar.y, 0.0001f));

    float4 sumDiffuse = 0.0f;
    float sumWeightDiffuse = 0.0f;
    float4 sumSpecular = 0.0f;
    float sumWeightSpecular = 0.0f;

    // À-Trous ウェーブレットカーネル 3x3 展開 (5x5 のほうが良いが、3x3 のほうが高性能)
    const int radius = 1;

    for (int y = -radius; y <= radius; ++y)
    {
        for (int x = -radius; x <= radius; ++x)
        {
            int2 offset = int2(x, y) * stepSize;
            int2 samplePos = clamp(int2(DTid.xy) + offset, int2(0, 0), int2(width - 1, height - 1));

            float4 sampleDiff = InputDiffuse[samplePos];
            float4 sampleSpec = InputSpecular[samplePos];
            if (passIndex == 0)
            {
                sampleDiff.rgb /= max(AlbedoMap[samplePos].rgb, 0.001f);
                
                // サンプル点の輝度安定化
                float sSpecLuma = dot(sampleSpec.rgb, LUMINANCE_VECTOR);
                sampleSpec.rgb /= (1.0f + sSpecLuma);
            }

            float3 sampleNormal = NormalMap[samplePos].xyz;
            float3 samplePosWorld = WorldPosMap[samplePos].xyz;

            // 1. 空間重み (Gaussian-like)
            float spatialWeight = exp(-(x * x + y * y) / 2.0f);

            // 2. 法線重み (★ stepSize に応じて制限を緩め、大きい Pass 時のモザイクを回避)
            float normalPower = clamp(128.0f / (float) (stepSize * stepSize), 2.0f, 128.0f);
            float normalWeight = pow(max(0.0f, dot(centerNormal, sampleNormal)), normalPower);

            // 3. 平面深度重み (★ stepSize に応じて緩め、曲面誤差を許容)
            float planeDist = abs(dot(centerNormal, centerPos - samplePosWorld));
            float posWeight = exp(-planeDist / (0.01f * stepSize + 0.001f));

            // ==========================================
            // 4. Diffuse 動的輝度重み (SVGF の核心)
            // ==========================================
            float sampleLumaDiff = dot(sampleDiff.rgb, LUMINANCE_VECTOR);
            // 差分 / (標準偏差 * 感度)。標準偏差が大きいほど指数が 0 に近づき、exp(0) = 1 (強制ブラー！)
            float lumaWeightDiff = exp(-abs(centerLumaDiff - sampleLumaDiff) / (stdDevDiff * 4.0f + 0.001f));
            
            float diffW = spatialWeight * normalWeight * posWeight * lumaWeightDiff;
            sumDiffuse += sampleDiff * diffW;
            sumWeightDiffuse += diffW;

            // ==========================================
            // 5. Specular 重み (Roughness と Specular 分散に誘導される)
            // ==========================================
            float sampleLumaSpec = dot(sampleSpec.rgb, LUMINANCE_VECTOR);
            float lumaWeightSpec = exp(-abs(centerLumaSpec - sampleLumaSpec) / (stdDevSpec * 4.0f + 0.001f));
            
            // 粗さが低いほど Specular は鏡面に近くなり、鮮明な反射を保つため空間拡散を小さくする必要がある
            float roughnessWeight = exp(-(x * x + y * y) / max(centerRoughness * centerRoughness * 2.0f, 0.05f));
            
            // 追加の保護機構：
            // 表面が非常に滑らか (Roughness < 0.1f) な場合、鏡のように振る舞うべき。
            // この場合、中心点以外 (x!=0 または y!=0) の重み寄与を強制的に削弱し、周辺ピクセルがハイライト反射を汚染しないようにする。
            if (centerRoughness < 0.1f && (x != 0 || y != 0))
            {
                roughnessWeight *= 0.05f; // 強力カットオフ
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
        // Albedo を復元 (Remodulation)
        finalDiffuse.rgb *= centerAlbedo;
        

        // ハイライト逆変調 (Inverse Tonemap)
        // 抑制されたエネルギーを復元し、PBR 金属反射が依然として輝くことを保証
        // ゼロ除算を防ぐため輝度の最大値を 0.99f に制限
        float fSpecLuma = clamp(dot(finalSpecular.rgb, LUMINANCE_VECTOR), 0.0f, 0.99f);
        finalSpecular.rgb /= (1.0f - fSpecLuma);
        
        float3 combined = finalDiffuse.rgb + finalSpecular.rgb;

        // トーンマッピングとガンマ補正
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