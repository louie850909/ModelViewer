RaytracingAccelerationStructure Scene : register(t0);
RWTexture2D<float4> RenderTarget : register(u0);

cbuffer CameraParams : register(b0)
{
    float4x4 viewProjInv;
    float3 cameraPos;
    float pad;
};

struct Payload
{
    float4 color;
};

[shader("raygeneration")]
void RayGen()
{
    uint2 launchIndex = DispatchRaysIndex().xy;
    uint2 launchDim = DispatchRaysDimensions().xy;

    // 將螢幕座標轉換到 NDC 空間 (-1 到 1)
    float2 d = (((float2) launchIndex + 0.5f) / (float2) launchDim) * 2.0f - 1.0f;
    d.y = -d.y;

    // 透過反矩陣將 NDC 解投影回世界空間，取得射線方向
    float4 target = mul(viewProjInv, float4(d.x, d.y, 1.0f, 1.0f));
    float3 rayDir = normalize((target.xyz / target.w) - cameraPos);

    RayDesc ray;
    ray.Origin = cameraPos;
    ray.Direction = rayDir;
    ray.TMin = 0.001f;
    ray.TMax = 10000.0f;

    Payload payload;
    payload.color = float4(0, 0, 0, 0);

    // 發射光線
    TraceRay(Scene, RAY_FLAG_NONE, 0xFF, 0, 1, 0, ray, payload);

    RenderTarget[launchIndex] = payload.color;
}

[shader("miss")]
void Miss(inout Payload payload)
{
    // 沒打中物件時的天空背景色
    payload.color = float4(0.2f, 0.4f, 0.8f, 1.0f);
}

[shader("closesthit")]
void ClosestHit(inout Payload payload, in BuiltInTriangleIntersectionAttributes attr)
{
    // 打中物件時，利用內建的重心座標上色
    float3 barycentrics = float3(1.0f - attr.barycentrics.x - attr.barycentrics.y, attr.barycentrics.x, attr.barycentrics.y);
    payload.color = float4(barycentrics, 1.0f);
}