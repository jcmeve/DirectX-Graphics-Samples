#include "AtmosphereCommon.hlsli"
#include "SkyViewCommon.hlsli"

static const int kSkySteps = 128;

Texture2D<float4> TransmittanceLut : register(t0);
Texture2D<float4> MultiScatteringLut : register(t1);
SamplerState LinearClampSampler : register(s0);

float4 main(float4 Position : SV_Position, float2 UV : TEXCOORD0) : SV_Target0
{

    //·¹ÀÌ¸¶Äª
    float3 Dir = GetWorldViewDir(UV);
    float3 Origin = CameraPositionKm;
    float CosTheta = dot(Dir, SunDirection);

    if (!MoveToTopAtmosphere(Origin, Dir))
    {
        float3 SpaceLuminance = (CosTheta > SunDiskCosAngle) ? SunIlluminance * SunDiskIntensity : 0.0;
        return float4(SpaceLuminance, 1.0);
    }

    float3 Throughput;
    float3 Ret = IntegrateScatteredLuminance(TransmittanceLut, MultiScatteringLut, LinearClampSampler,
        Origin, Dir, SunDirection, SunIlluminance, MultiScatteringFactor, kSkySteps, kInfiniteDistance, false, Throughput);

    bool bIntersectGround = RaySphereNear(Origin, Dir, BottomRadius) > 0.0;
    if (!bIntersectGround && CosTheta > SunDiskCosAngle)
        Ret += Throughput * SunIlluminance * SunDiskIntensity;

    return float4(Ret, 1.0);
}