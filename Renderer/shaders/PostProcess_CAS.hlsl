Texture2D<float4> InputColor : register(t0);
RWTexture2D<float4> OutputColor : register(u0);

cbuffer CASConstants : register(b0)
{
    float Sharpness; // シャープネス強度、推奨範囲 0.0 (なし) ～ 1.0 (最強)
    int Width;
    int Height;
    float padding;
};

// 輝度計算
float Min3(float a, float b, float c)
{
    return min(a, min(b, c));
}
float Max3(float a, float b, float c)
{
    return max(a, max(b, c));
}

[numthreads(8, 8, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID)
{
    int2 pos = int2(DTid.xy);
    if (pos.x >= Width || pos.y >= Height)
        return;

    // 十字型の隣接ピクセルをサンプリング
    //   b
    // d e f
    //   h
    float3 a = InputColor[clamp(pos + int2(0, -1), 0, int2(Width - 1, Height - 1))].rgb; // b
    float3 b = InputColor[clamp(pos + int2(-1, 0), 0, int2(Width - 1, Height - 1))].rgb; // d
    float3 c = InputColor[pos].rgb; // e (中心点)
    float3 d = InputColor[clamp(pos + int2(1, 0), 0, int2(Width - 1, Height - 1))].rgb; // f
    float3 e = InputColor[clamp(pos + int2(0, 1), 0, int2(Width - 1, Height - 1))].rgb; // h

    // ローカル領域の RGB 最小値と最大値を計算
    float3 minRGB = Min3(Min3(a, b, c), d, e);
    float3 maxRGB = Max3(Max3(a, b, c), d, e);

    // ゼロ除算を防ぐ
    maxRGB = max(maxRGB, 0.00001f);

    // 重みを計算：局所コントラストが高いほどシャープネス重みが低くなり、エッジのリンギングを防ぐ
    float3 ampLimit = min(minRGB, 1.0f - maxRGB);
    float3 w = ampLimit / maxRGB;
    
    // 最終的なシャープネス強度を制御
    // Sharpness は 0 (なし) から 1 (最大) まで。CAS 内部パラメータへの変換式：
    float CAS_Param = lerp(-0.125f, -0.2f, clamp(Sharpness, 0.0f, 1.0f));
    w *= CAS_Param;

    // フィルタを適用： center + neighbors * weight
    float3 outputRGB = (a * w + b * w + d * w + e * w + c) / (1.0f + 4.0f * w);

    // 出力、元の Alpha を保持 (存在する場合)
    OutputColor[pos] = float4(saturate(outputRGB), InputColor[pos].a);
}