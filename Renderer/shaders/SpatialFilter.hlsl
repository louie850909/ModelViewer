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
        //centerDiff.rgb /= centerAlbedo;
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

    // À-Trous ウェーブレットカーネル 5x5 展開 (IBL ノイズ対応のため 3x3 から拡大)
    const int radius = 2;

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
                //sampleDiff.rgb /= max(AlbedoMap[samplePos].rgb, 0.001f);
                
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
            // 暗部対応: 分母に「信号強度に比例する下限」を追加。
            // 暗部 (center/sample luma が小さい) では σ も小さいので、絶対差分ベースの
            // 重みだと鄰近を過度に拒絶してノイズが残る。相対スケールを導入して救済。
            float lumaScale = max(centerLumaDiff, sampleLumaDiff) * 0.5f;
            float lumaWeightDiff = exp(-abs(centerLumaDiff - sampleLumaDiff) / (stdDevDiff * 8.0f + lumaScale + 0.1f));
            
            float diffW = spatialWeight * normalWeight * posWeight * lumaWeightDiff;
            sumDiffuse += sampleDiff * diffW;
            sumWeightDiffuse += diffW;

            // ==========================================
            // 5. Specular 重み (Roughness と Specular 分散に誘導される)
            // ==========================================
            float sampleLumaSpec = dot(sampleSpec.rgb, LUMINANCE_VECTOR);
            float lumaScaleSpec = max(centerLumaSpec, sampleLumaSpec) * 0.5f;
            float lumaWeightSpec = exp(-abs(centerLumaSpec - sampleLumaSpec) / (stdDevSpec * 8.0f + lumaScaleSpec + 0.1f));

            // Specular カーネル: 純 roughness 基準で安定化。
            // σ に依存させるとフレーム毎にカーネルが変化し、時間的なちらつき (跳動) を誘発。
            // 鏡面/屈折の噪声は時間積分 (TemporalAccumulation) に委ねる。
            float specKernelScale = max(centerRoughness * centerRoughness * 2.0f, 0.05f);
            float roughnessWeight = exp(-(x * x + y * y) / specKernelScale);

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