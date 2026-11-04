#ifndef ATMOSPHERE_CLOUD_SHADOW_HLSLI
#define ATMOSPHERE_CLOUD_SHADOW_HLSLI

#include "SkyViewCommon.hlsli"
#include "CloudShadowCommon.hlsli"

Texture2D<float4> CloudShadowMap : register(t5);

float GetCloudOcclusion(SamplerState LinearClamp, float3 PosKm)
{
    float3 PosMeter = float3(PosKm.x, PosKm.y - BottomRadius, PosKm.z) * 1000.0;
    return 1.0 - GetCloudShadowTransmittance(CloudShadowMap, LinearClamp, PosMeter);
}

float3 SampleAtmosphereShadow(Texture3D<float4> Volume, SamplerState LinearClamp, float2 UV, float DistanceKm)
{
    if (CloudShadowShaftMaxDistanceKm <= 0)
        return 0.0;
    float W = sqrt(saturate(DistanceKm / CloudShadowShaftMaxDistanceKm));
    return Volume.Sample(LinearClamp, float3(UV, W), 0).rgb;
}

#endif