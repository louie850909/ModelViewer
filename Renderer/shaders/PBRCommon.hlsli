#ifndef PBR_COMMON_HLSLI
#define PBR_COMMON_HLSLI

static const float PI = 3.14159265359;

float DistributionGGX(float3 N, float3 H, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    return a2 / max(PI * denom * denom, 0.0000001);
}

float GeometrySchlickGGX(float NdotV, float roughness)
{
    float r = (roughness + 1.0);
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float GeometrySmith(float3 N, float3 V, float3 L, float roughness)
{
    return GeometrySchlickGGX(max(dot(N, V), 0.0), roughness)
         * GeometrySchlickGGX(max(dot(N, L), 0.0), roughness);
}

float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// ==========================================
// Phase 2: GGX VNDF Core Functions
// ==========================================

// 接線空間行列 (TBN) を構築
float3x3 GetTBN(float3 n)
{
    float3 up = abs(n.z) < 0.999f ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 t = normalize(cross(up, n));
    float3 b = cross(n, t);
    return float3x3(t, b, n); // HLSL では t, b, n が行列の 3 つの Row に設定される
}

// GGX VNDF (Visible Normal Distribution Function) サンプリング
// 参考: Eric Heitz, "Sampling the GGX Distribution of Visible Normals", JCGT 2018
float3 SampleGGXVNDF(float3 Ve, float alpha_x, float alpha_y, float U1, float U2)
{
    // 1. 視線方向を半球配置 (Hemisphere configuration) に変換
    float3 Vh = normalize(float3(alpha_x * Ve.x, alpha_y * Ve.y, Ve.z));

    // 2. 正規直交基底 (Orthonormal basis) を構築
    float lensq = Vh.x * Vh.x + Vh.y * Vh.y;
    float3 T1 = lensq > 0.0f ? float3(-Vh.y, Vh.x, 0.0f) * rsqrt(lensq) : float3(1.0f, 0.0f, 0.0f);
    float3 T2 = cross(Vh, T1);

    // 3. 投影面積のパラメータ化
    float r = sqrt(U1);
    float phi = 2.0f * PI * U2;
    float t1 = r * cos(phi);
    float t2 = r * sin(phi);
    float s = 0.5f * (1.0f + Vh.z);
    t2 = (1.0f - s) * sqrt(max(0.0f, 1.0f - t1 * t1)) + s * t2;

    // 4. 半球に再投影
    float3 Nh = t1 * T1 + t2 * T2 + sqrt(max(0.0f, 1.0f - t1 * t1 - t2 * t2)) * Vh;

    // 5. 法線を楕円体配置 (Ellipsoid configuration) に変換して戻す
    return normalize(float3(alpha_x * Nh.x, alpha_y * Nh.y, max(0.0f, Nh.z)));
}

#endif