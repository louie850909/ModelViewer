#ifndef LIGHT_HLSLI
#define LIGHT_HLSLI

struct Light
{
    int type;
    float intensity;
    float coneAngle;
    float _pad;
    float3 color;
    float _pad2;
    float3 position;
    float _pad3;
    float3 direction;
    float _pad4;
};

cbuffer LightBuffer : register(b1)
{
    int numLights;
    float3 cameraPos;
    Light lights[16];
};

#endif