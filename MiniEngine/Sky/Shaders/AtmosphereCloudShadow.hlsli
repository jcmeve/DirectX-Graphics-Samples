#ifndef ATMOSPHERE_CLOUD_SHADOW_HLSLI
#define ATMOSPHERE_CLOUD_SHADOW_HLSLI

#include "SkyViewCommon.hlsli"
#include "CloudShadowCommon.hlsli"

Texture2D<float4> CloudShadowMap : register(t5);

static const uint kShaftStepCount = 32;

// 구름 그림자에 가려진 대기 산란량 = Sigma (구간별 AerialPerspective × (1 - 투과율))
float3 GetCloudShadowedInScattering(Texture3D<float4> Volume, SamplerState LinearClamp, float2 UV, float3 Dir, float DistanceKm, float Jitter)
{
    float MarchKm = min(DistanceKm, CloudShadowShaftMaxDistanceKm);
    if (MarchKm <= 0.0)
        return 0.0;

    float StepKm = MarchKm / kShaftStepCount;
    float3 Loss = 0.0;
    float3 Prev = 0.0;
    for (uint i = 0; i < kShaftStepCount; ++i)
    {
        float3 Next = SampleAerialPerspective(Volume, LinearClamp, UV, (i + 1) * StepKm).rgb;
        float3 PosKm = CameraPositionKm + Dir * ((i + Jitter) * StepKm);
        float3 r = float3(PosKm.x, length(PosKm) - BottomRadius, PosKm.z) * 1000.0;
        Loss += (Next - Prev) * (1.0 - GetCloudShadowTransmittance(CloudShadowMap, LinearClamp, r));
        Prev = Next;
    }
    return Loss;
}

#endif