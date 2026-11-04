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

float3 IntegrateScatteredLuminanceLoss(Texture2D<float4> TransmittanceLut, Texture2D<float4> MultiScatteringLut, 
    SamplerState LinearClamp, float3 Origin, float3 Dir, float3 SunDir,
    float SunIlluminance, float MultiScatteringFactor, int StepCount,
    float MaxDistance, bool bUseSunVisibility, out float3 Throughput)
{
    Throughput = 1.0;

    float TopDistance = RaySphereFar(Origin, Dir, TopRadius);
    if (TopDistance <= 0.0)
        return 0.0;

    float GroundDistance = RaySphereNear(Origin, Dir, BottomRadius);
    float RayLength = (GroundDistance > 0.0) ? GroundDistance : TopDistance;
    RayLength = min(RayLength, MaxDistance);
    
    float CosTheta = dot(Dir, SunDir);
    float PhaseR = RayleighPhase(CosTheta);
    float PhaseM = MiePhase(CosTheta, MiePhaseG);

    float StepSize = RayLength / StepCount;
    float3 Luminance = 0.0;

    for (int i = 0; i < StepCount; ++i)
    {
        float3 SamplePos = Origin + Dir * ((i + 0.5) * StepSize);
        float r = length(SamplePos);
        OpticalCoefficients Coefficients = GetOpticalCoefficients(r - BottomRadius);

        float SunMu = dot(SamplePos / r, SunDir); //천정각
        float PlanetShadow = (RaySphereNear(SamplePos, SunDir, BottomRadius) > 0.0) ? 0.0 : 1.0; //행성과 닿은 경우 태양 가시성 0
        float3 SunTransmittance = SampleTransmittance(TransmittanceLut, LinearClamp, r, SunMu) * PlanetShadow;
        if (bUseSunVisibility)
        {
            float3 PosKm = SamplePos;//CameraPositionKm + Dir * ((i + Jitter) * StepKm);
            float3 PosMeter = float3(PosKm.x, length(PosKm) - BottomRadius, PosKm.z) * 1000.0;
            SunTransmittance *= 1 - GetCloudShadowTransmittance(CloudShadowMap, LinearClamp, PosMeter);
        }
        
        float3 MultiScattering = SampleMultiScattering(MultiScatteringLut, LinearClamp, r, SunMu) * MultiScatteringFactor;
        
        float3 InScattering = SunIlluminance * (
              SunTransmittance * (Coefficients.RayleighScattering * PhaseR + Coefficients.MieScattering * PhaseM)
            + MultiScattering * (Coefficients.RayleighScattering + Coefficients.MieScattering)); //

        float3 StepTransmittance = exp(-Coefficients.TotalExtinction * StepSize); //구간 투과율
        Luminance += Throughput * (InScattering - InScattering * StepTransmittance) / max(Coefficients.TotalExtinction, 1e-6);
        Throughput *= StepTransmittance;
    }

    return Luminance;
}

#endif