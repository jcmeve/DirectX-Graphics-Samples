#ifndef SKY_VIEW_COMMON_HLSLI
#define SKY_VIEW_COMMON_HLSLI

#include "AtmosphereCommon.hlsli"

cbuffer SkyViewConstants : register(b2)
{
    float4x4 InvViewProj;
    
    float3 CameraPositionKm;
    float SunIlluminance;
    
    float3 SunDirection;
    float SunDiskCosAngle;
    
    float SunDiskIntensity;
    float MultiScatteringFactor;
    float AerialPerspectiveWorldToKm;
    float SkyViewConstantsPad;
};


//UV에서 거꾸로 월드 공간 벡터로 변환
float3 GetWorldViewDir(float2 UV)
{
    float2 Ndc = float2(UV.x * 2.0 - 1.0, 1.0 - UV.y * 2.0);
    float4 NearPos = mul(InvViewProj, float4(Ndc, 1.0, 1.0));
    float4 FarPos = mul(InvViewProj, float4(Ndc, 0.5, 1.0));
    return normalize(FarPos.xyz / FarPos.w - NearPos.xyz / NearPos.w);
}

//UV, Depth로 원래 포지션 계산, Near Face와의 거리 계산
float GetSceneViewDistance(float2 UV, float Depth)
{
    if (Depth <= 0.0)
        return kInfiniteDistance;

    float2 Ndc = float2(UV.x * 2.0 - 1.0, 1.0 - UV.y * 2.0);
    float4 SurfacePos = mul(InvViewProj, float4(Ndc, Depth, 1.0));
    float4 NearPos = mul(InvViewProj, float4(Ndc, 1.0, 1.0));
    return length(SurfacePos.xyz / SurfacePos.w - NearPos.xyz / NearPos.w);
}

#endif