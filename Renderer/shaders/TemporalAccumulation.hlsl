cbuffer Constants : register(b0)
{
    uint width;
    uint height;
    float3 cameraPos;
    float _pad;
    float4x4 prevViewProj;
};

RWTexture2D<float4> OutputDiffuse : register(u0);
RWTexture2D<float4> OutputSpecular : register(u1);
RWTexture2D<float4> OutputNormal : register(u2);
RWTexture2D<float4> OutputPos : register(u3);
RWTexture2D<float2> OutputVariance : register(u4);

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

// 輝度計算用のヘルパー関数
float GetLuminance(float3 color)
{
    return dot(color, float3(0.2126f, 0.7152f, 0.0722f));
}

// 動態ベクトルの膨張 (Velocity Dilation)
float2 GetDilatedVelocity(int2 pos)
{
    float closestDist = 10000000.0f;
    float2 dilatedVel = VelocityMap[pos];

    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            int2 samplePos = clamp(pos + int2(x, y), int2(0, 0), int2(width - 1, height - 1));
            float3 n = CurrentNormal[samplePos].xyz;
            
            // 実際にジオメトリが存在するピクセルの動態ベクトルのみ考慮 (空のゼロベクトル汚染を排除)
            if (length(n) > 0.1f)
            {
                float3 wPos = CurrentPos[samplePos].xyz;
                float dist = length(wPos - cameraPos);
                if (dist < closestDist)
                {
                    closestDist = dist;
                    dilatedVel = VelocityMap[samplePos];
                }
            }
        }
    }
    return dilatedVel;
}

// Catmull-Rom 双三次補間関数
float4 SampleTextureCatmullRom(Texture2D<float4> tex, SamplerState linearSampler, float2 uv, float2 texSize)
{
    float2 samplePos = uv * texSize;
    float2 texPos1 = floor(samplePos - 0.5f) + 0.5f;
    float2 f = samplePos - texPos1;

    // Catmull-Rom 重みを計算
    float2 w0 = f * (-0.5f + f * (1.0f - 0.5f * f));
    float2 w1 = 1.0f + f * f * (-2.5f + 1.5f * f);
    float2 w2 = f * (0.5f + f * (2.0f - 1.5f * f));
    float2 w3 = f * f * (-0.5f + 0.5f * f);

    float2 w12 = w1 + w2;
    float2 offset12 = w2 / max(w12, 0.00001f);

    float2 texPos0 = texPos1 - 1.0f;
    float2 texPos3 = texPos1 + 2.0f;
    float2 texPos12 = texPos1 + offset12;

    // UV 空間に正規化
    texPos0 /= texSize;
    texPos3 /= texSize;
    texPos12 /= texSize;

    float4 result = 0.0f;
    // 5 回のハードウェア双線形サンプリング
    result += tex.SampleLevel(linearSampler, float2(texPos12.x, texPos12.y), 0) * w12.x * w12.y;
    result += tex.SampleLevel(linearSampler, float2(texPos0.x, texPos12.y), 0) * w0.x * w12.y;
    result += tex.SampleLevel(linearSampler, float2(texPos3.x, texPos12.y), 0) * w3.x * w12.y;
    result += tex.SampleLevel(linearSampler, float2(texPos12.x, texPos0.y), 0) * w12.x * w0.y;
    result += tex.SampleLevel(linearSampler, float2(texPos12.x, texPos3.y), 0) * w12.x * w3.y;

    return max(result, 0.0f);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= width || DTid.y >= height)
        return;

    float2 uv = (float2(DTid.xy) + 0.5f) / float2(width, height);
    
    // 直接読み取りの代わりに Dilation を使用
    float2 velocity = GetDilatedVelocity(DTid.xy);

    float4 currDiffuse = RawDiffuse[DTid.xy];
    float4 currSpecular = RawSpecular[DTid.xy];
    float4 normalRoughness = CurrentNormal[DTid.xy];
    float3 centerNormal = normalRoughness.xyz;
    float centerRoughness = normalRoughness.w;
    // GeometryPass で透過材質は roughness=0.02 を書き込むマーカーとして使っているため、
    // 0.05 未満を透過ピクセルとして扱う。
    bool isTransmission = (centerRoughness < 0.05f) && (length(centerNormal) > 0.1f);
    float3 centerPos = CurrentPos[DTid.xy].xyz;

    OutputNormal[DTid.xy] = float4(centerNormal, 1.0f);
    OutputPos[DTid.xy] = float4(centerPos, 1.0f);

    float2 prevUV = uv - velocity;
    float4 histDiffuse = currDiffuse;
    float4 histSpecular = currSpecular;
    float historyValid = 0.0f;

    float3 m1_diff = 0.0f, m2_diff = 0.0f;
    float3 m1_spec = 0.0f, m2_spec = 0.0f;
    float centerLumaDiff = GetLuminance(currDiffuse.rgb);
    float centerLumaSpec = GetLuminance(currSpecular.rgb);

    // ── Neighborhood Min/Max (中心を除く 8 近傍) ──
    // Firefly を殺すための古典的手法。中心を除外した鄰域の min/max を計算し、
    // 現在ピクセルがこの範囲を超えていれば outlier (firefly) として clamp する。
    // 3x3 σ ベースの clamp と違い、屈折のような非平滑な信号でも鄰域の「実際の範囲」を
    // 使うので、統計量の膨張に引きずられない。
    float3 neighborMinDiff =  1e10f, neighborMaxDiff = -1e10f;
    float3 neighborMinSpec =  1e10f, neighborMaxSpec = -1e10f;

    for (int ky = -1; ky <= 1; ky++)
    {
        for (int kx = -1; kx <= 1; kx++)
        {
            int2 samplePos = clamp(int2(DTid.xy) + int2(kx, ky), int2(0, 0), int2(width - 1, height - 1));

            float3 rawDiff = RawDiffuse[samplePos].rgb;
            float3 rawSpec = RawSpecular[samplePos].rgb;

            // 絶対 Firefly 上限
            const float maxLumaDiff = 6.0f;
            const float maxLumaSpec = 6.0f;

            float diffLuma = GetLuminance(rawDiff);
            if (diffLuma > maxLumaDiff)
                rawDiff *= (maxLumaDiff / diffLuma);

            float specLuma = GetLuminance(rawSpec);
            if (specLuma > maxLumaSpec)
                rawSpec *= (maxLumaSpec / specLuma);

            float3 c_diff = RGBToYCoCg(rawDiff);
            m1_diff += c_diff;
            m2_diff += c_diff * c_diff;

            float3 c_spec = RGBToYCoCg(rawSpec);
            m1_spec += c_spec;
            m2_spec += c_spec * c_spec;

            // 中心以外の 8 近傍で min/max を集計 (中心のアウトライヤーを除外)
            if (kx != 0 || ky != 0)
            {
                neighborMinDiff = min(neighborMinDiff, c_diff);
                neighborMaxDiff = max(neighborMaxDiff, c_diff);
                neighborMinSpec = min(neighborMinSpec, c_spec);
                neighborMaxSpec = max(neighborMaxSpec, c_spec);
            }
        }
    }

    float3 mu_diff = m1_diff / 9.0f;
    float3 sigma_diff = sqrt(max(m2_diff / 9.0f - mu_diff * mu_diff, 0.0f));
    float3 mu_spec = m1_spec / 9.0f;
    float3 sigma_spec = sqrt(max(m2_spec / 9.0f - mu_spec * mu_spec, 0.0f));

    float varianceDiff = sigma_diff.x;
    float varianceSpec = sigma_spec.x;

    // ── 透過ピクセル専用処理 ──
    // シルエットでは 3x3 鄰居に空/背景が混ざり、jitter に応じて有効鄰居数が変動する。
    // + 3x3 の平均でも jitter 変異が残るため、透過ピクセルだけ 7x7 (最大 49 サンプル)
    //   の範囲で同じガラス面上の鄰居だけで平均を取り、フレーム間安定性を最大化する。
    //   ガラスは画面の小部分のため 7x7 のコストは許容範囲。
    if (isTransmission)
    {
        float3 wideSum = 0.0f;
        float wideCount = 0.0f;
        for (int wy = -3; wy <= 3; wy++)
        {
            for (int wx = -3; wx <= 3; wx++)
            {
                int2 sp = clamp(int2(DTid.xy) + int2(wx, wy), int2(0, 0), int2(width - 1, height - 1));
                float4 nr = CurrentNormal[sp];
                // 同じガラス面のみ採用
                if (nr.w < 0.05f && length(nr.xyz) > 0.1f)
                {
                    // 法線が大きく異なる (曲面の反対側など) 場合は除外
                    if (dot(nr.xyz, centerNormal) > 0.7f)
                    {
                        float3 s = RawSpecular[sp].rgb;
                        // 各サンプルに硬 luma 上限 (単発 sun 命中の寄与を制限)
                        float sL = GetLuminance(s);
                        if (sL > 3.0f)
                            s *= (3.0f / sL);
                        wideSum += s;
                        wideCount += 1.0f;
                    }
                }
            }
        }
        if (wideCount > 0.0f)
        {
            currSpecular.rgb = wideSum / wideCount;
            // 最終 luma 硬 clamp
            float tLuma = GetLuminance(currSpecular.rgb);
            if (tLuma > 1.5f)
                currSpecular.rgb *= (1.5f / tLuma);
        }
    }

    // ── Firefly Killer (入力側) — Neighborhood Min/Max Clamp ──
    // 8 近傍の min/max 範囲に現在ピクセルを硬 clamp する。
    // 鄰域が outlier を含まない限り、中心の outlier は確実に除去される。
    // 屈折のような非平滑信号でも、鄰域の「現実的な範囲」を超える値のみ狙い撃ちで弾く。
    // 範囲を少し広げる余裕 (1.1x) を設けて、正常なハイライトを誤殺しないように。
    {
        // Diffuse は少し余裕 (10%) を残す
        float3 rangeDiff = neighborMaxDiff - neighborMinDiff;
        float3 loDiff = neighborMinDiff - rangeDiff * 0.1f;
        float3 hiDiff = neighborMaxDiff + rangeDiff * 0.1f;
        float3 cY_diff = RGBToYCoCg(currDiffuse.rgb);
        cY_diff = clamp(cY_diff, loDiff, hiDiff);
        currDiffuse.rgb = max(YCoCgToRGB(cY_diff), 0.0f);

        // Specular は硬 clamp (拡張なし)。中心がアウトライヤーなら完全に殺す。
        float3 cY_spec = RGBToYCoCg(currSpecular.rgb);
        cY_spec = clamp(cY_spec, neighborMinSpec, neighborMaxSpec);
        currSpecular.rgb = max(YCoCgToRGB(cY_spec), 0.0f);
    }

    if (all(prevUV >= 0.0f) && all(prevUV <= 1.0f))
    {
        float3 hNormal = HistoryNormal.SampleLevel(LinearSampler, prevUV, 0).xyz;
        float3 hPos = HistoryPos.SampleLevel(LinearSampler, prevUV, 0).xyz;

        // 空とオブジェクトのハードマスク
        bool isSky = length(centerNormal) < 0.1f;
        bool wasSky = length(hNormal) < 0.1f;

        if (isSky != wasSky)
        {
            // 現在のピクセルが時間軸で空とオブジェクトの切り替えが発生、履歴を強制切断！
            historyValid = 0.0f;
        }
        else if (isSky)
        {
            // 両方とも空：空はノイズのないベタ塗りのため、ブラーを避けるため履歴を直接破棄
            historyValid = 0.0f;
        }
        else
        {
            // 通常の実体オブジェクトの滑らかな減衰
            float normalDist = saturate(dot(centerNormal, hNormal));
            
            // カメラからの相対距離差を導入、エッジの「双線形補間」による深度崩壊に非常に有効
            float centerDist = length(centerPos - cameraPos);
            float hDist = length(hPos - cameraPos);
            float distDiff = abs(centerDist - hDist) / max(centerDist, 0.001f);
            
            float planeDist = abs(dot(centerNormal, centerPos - hPos));

            float normalWeight = exp(-(1.0f - normalDist) * 8.0f);
            float depthWeight = exp(-planeDist * 15.0f) * exp(-distDiff * 20.0f); // 二重防御

            historyValid = saturate(normalWeight * depthWeight);
        }

        if (historyValid > 0.01f)
        {
            float2 texSize = float2(width, height);
            histDiffuse = SampleTextureCatmullRom(HistoryDiffuse, LinearSampler, prevUV, texSize);
            histSpecular = SampleTextureCatmullRom(HistorySpecular, LinearSampler, prevUV, texSize);

            // Camera 速度ベースで clamp 強度を調整:
            // 静止時はほぼ clamp なし (広い 10σ) にして履歴を完全に信頼 → ちらつき排除。
            // 動いている時は狭めの clamp で ghosting を防ぐ。
            float velMagCl = length(velocity);
            float clampTightness = lerp(10.0f, 2.5f, saturate(velMagCl * 200.0f));
            float clampTightnessSpec = lerp(10.0f, 4.0f, saturate(velMagCl * 200.0f));

            float3 histY_diff = RGBToYCoCg(histDiffuse.rgb);
            histY_diff = clamp(histY_diff, mu_diff - clampTightness * sigma_diff, mu_diff + clampTightness * sigma_diff);
            histDiffuse = float4(YCoCgToRGB(histY_diff), histDiffuse.a);

            float3 histY_spec = RGBToYCoCg(histSpecular.rgb);
            histY_spec = clamp(histY_spec, mu_spec - clampTightnessSpec * sigma_spec, mu_spec + clampTightnessSpec * sigma_spec);
            histSpecular = float4(YCoCgToRGB(histY_spec), histSpecular.a);
        }
    }

    float velMag = length(velocity);
    // 静止時の base blend: diffuse は 0.05 (~20 frame EMA)、
    // specular は鏡面/屈折のため更に低い 0.03 (~33 frame EMA) で長時間積分する。
    float baseBlendDiff = lerp(0.05f, 0.30f, saturate(velMag * 100.0f));
    float baseBlendSpec = lerp(0.03f, 0.30f, saturate(velMag * 100.0f));

    float adaptDiff = baseBlendDiff * lerp(0.7f, 1.3f, saturate(1.0f - varianceDiff * 4.0f));
    adaptDiff = clamp(adaptDiff, 0.02f, 0.50f);

    float adaptSpec = baseBlendSpec * lerp(0.7f, 1.3f, saturate(1.0f - varianceSpec * 4.0f));
    adaptSpec = clamp(adaptSpec, 0.015f, 0.50f);

    // 透過ピクセルは静止時に長い EMA (100 フレーム) を使って ちらつきを消すが、
    // カメラ移動時は速度に応じて cap を緩めて応答性を確保する。
    // 静止: 0.01 → 100f EMA、高速移動: 0.30 → 通常の応答性。
    if (isTransmission)
    {
        float transCap = lerp(0.01f, 0.30f, saturate(velMag * 100.0f));
        adaptSpec = min(adaptSpec, transCap);
    }

    float blendDiff = lerp(1.0f, adaptDiff, historyValid);
    float blendSpec = lerp(1.0f, adaptSpec, historyValid);

    OutputDiffuse[DTid.xy] = lerp(histDiffuse, currDiffuse, blendDiff);
    OutputSpecular[DTid.xy] = lerp(histSpecular, currSpecular, blendSpec);
    OutputVariance[DTid.xy] = float2(varianceDiff, varianceSpec);
}