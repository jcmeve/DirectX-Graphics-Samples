#include "AtmosphereCloudShadow.hlsli"

Texture2D<float4> TransmittanceLut : register(t0);
Texture2D<float4> SkyViewLut : register(t2);
Texture3D<float4> AerialPerspectiveVolume : register(t3);
SamplerState LinearClampSampler : register(s0);

float4 main(float4 Position : SV_Position, float2 UV : TEXCOORD0) : SV_TARGET0
{
    float3 Dir = GetWorldViewDir(UV);
    float ViewHeight = length(CameraPositionKm);
    float3 Up = CameraPositionKm / ViewHeight;
	
    float ViewZenithCos = dot(Dir, Up);
	
    float LightViewCos = 1.0;
    float3 Side = cross(Up, Dir);
    if (dot(Side, Side) > 1e-8)
    {
        Side = normalize(Side);
        float3 Forward = normalize(cross(Side, Up)); // 시선의 수평 방향
        float2 LightOnPlane = float2(dot(SunDirection, Forward), dot(SunDirection, Side));
        float LightOnPlaneLength = length(LightOnPlane);
        if (LightOnPlaneLength > 1e-6)
            LightViewCos = LightOnPlane.x / LightOnPlaneLength;
    }

    bool bIntersectGround = RaySphereNear(CameraPositionKm, Dir, BottomRadius) >= 0.0;
    
    float2 LutUV = SkyViewParamsToUv(bIntersectGround, ViewZenithCos, LightViewCos, ViewHeight);
    float3 Luminance = SkyViewLut.SampleLevel(LinearClampSampler, LutUV, 0).rgb;

    // 구름 그림자 빛줄기
    float Jitter = InterleavedGradientNoise(Position.xy, CloudShadowFrameIndex);
    Luminance = max(Luminance - GetCloudShadowedInScattering(AerialPerspectiveVolume, LinearClampSampler, UV, Dir, kAerialPerspectiveMaxDistanceKm, Jitter), 0.0);
    
    if (!bIntersectGround && dot(Dir, SunDirection) > SunDiskCosAngle)
        Luminance += SampleTransmittance(TransmittanceLut, LinearClampSampler, ViewHeight, ViewZenithCos)
            * SunIlluminance * SunDiskIntensity;

    return float4(Luminance, 1.0);
}