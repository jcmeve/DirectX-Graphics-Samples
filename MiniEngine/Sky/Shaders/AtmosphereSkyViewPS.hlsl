#include "AtmosphereCommon.hlsli"
#include "SkyViewCommon.hlsli"

Texture2D<float4> TransmittanceLut : register(t0);
Texture2D<float4> SkyViewLut : register(t2);
SamplerState LinearClampSampler : register(s0);

float4 main(float4 Position : SV_Position, float2 UV : TEXCOORD0) : SV_TARGET0
{
	float3 Dir = GetWorldViewDir(UV);
    float ViewHeight = length(CameraPositionKm);
    float3 Up = CameraPositionKm / ViewHeight;
	
    float ViewZenithCos = dot(Dir, Up);
	
    // 천정을 보거나 태양이 천정에 있으면 방위가 의미 없으므로 1(태양 쪽)로 둠
    float LightViewCos = 1.0;
    float3 Side = cross(Up, Dir);
    if (dot(Side, Side) > 1e-8)
    {
        Side = normalize(Side);
        float3 Forward = normalize(cross(Side, Up)); // 시선의 수평 성분 방향
        float2 LightOnPlane = float2(dot(SunDirection, Forward), dot(SunDirection, Side));
        float LightOnPlaneLength = length(LightOnPlane);
        if (LightOnPlaneLength > 1e-6)
            LightViewCos = LightOnPlane.x / LightOnPlaneLength;
    }

    bool bIntersectGround = RaySphereNear(CameraPositionKm, Dir, BottomRadius) >= 0.0;
    
    float2 LutUV = SkyViewParamsToUv(bIntersectGround, ViewZenithCos, LightViewCos, ViewHeight);
    float3 Luminance = SkyViewLut.SampleLevel(LinearClampSampler, LutUV, 0).rgb;
    
    if (!bIntersectGround && dot(Dir, SunDirection) > SunDiskCosAngle)
        Luminance += SampleTransmittance(TransmittanceLut, LinearClampSampler, ViewHeight, ViewZenithCos)
            * SunIlluminance * SunDiskIntensity;

    return float4(Luminance, 1.0);

	
	

}