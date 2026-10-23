#include "AtmosphereCommon.hlsli"
#include "SkyViewCommon.hlsli"

static const int kSkySteps = 128;

Texture2D<float4> TransmittanceLut : register(t0);
Texture2D<float4> MultiScatteringLut : register(t1);
SamplerState LinearClampSampler : register(s0);

float4 main(float4 Position : SV_Position, float2 UV : TEXCOORD0) : SV_Target0
{
    float3 Dir = GetWorldViewDir(UV);
    float3 Origin = CameraPositionKm;
    float CosTheta = dot(Dir, SunDirection);
    
    if (!MoveToTopAtmosphere(Origin, Dir))
    {
        float3 SpaceLuminance = (CosTheta > SunDiskCosAngle) ? SunIlluminance * SunDiskIntensity : 0.0;
        return float4(SpaceLuminance, 1.0);
    }
    
    float TopDistance = RaySphereFar(Origin, Dir, TopRadius);
    if (TopDistance <= 0.0)
        return float4(0.0, 0.0, 0.0, 1.0);

    float GroundDistance = RaySphereNear(Origin, Dir, BottomRadius);
    float RayLength = (GroundDistance > 0.0) ? GroundDistance : TopDistance;

    float PhaseR = RayleighPhase(CosTheta);
    float PhaseM = MiePhase(CosTheta, MiePhaseG);

    float StepSize = RayLength / kSkySteps;
    float3 Luminance = 0.0;
    float3 Throughput = 1.0;

    //레이마칭
    for (int i = 0; i < kSkySteps; ++i)
    {
        float3 SamplePos = Origin + Dir * ((i + 0.5) * StepSize);
        float r = length(SamplePos);
        OpticalCoefficients Coefficients = GetOpticalCoefficients(r - BottomRadius);

        // LUT에서 태양 까지 빛의 투과율 얻어옴
        float SunMu = dot(SamplePos / r, SunDirection); //천정각
        float PlanetShadow = (RaySphereNear(SamplePos, SunDirection, BottomRadius) > 0.0) ? 0.0 : 1.0;
        float3 SunTransmittance = SampleTransmittance(TransmittanceLut, LinearClampSampler, r, SunMu) * PlanetShadow;

        //산란된 빛이 추가됨
        float3 MultiScattering = SampleMultiScattering(MultiScatteringLut, LinearClampSampler, r, SunMu) * MultiScatteringFactor;
        float3 InScattering = SunIlluminance * (
              SunTransmittance * (Coefficients.RayleighScattering * PhaseR + Coefficients.MieScattering * PhaseM)
            + MultiScattering * (Coefficients.RayleighScattering + Coefficients.MieScattering));
        
        
        // 적분
        float3 StepTransmittance = exp(-Coefficients.TotalExtinction * StepSize);
        Luminance += Throughput * (InScattering - InScattering * StepTransmittance) / max(Coefficients.TotalExtinction, 1e-6);
        Throughput *= StepTransmittance;
    }

    // 태양 자체
    if (GroundDistance < 0.0 && CosTheta > SunDiskCosAngle)
        Luminance += Throughput * SunIlluminance * SunDiskIntensity;

    return float4(Luminance, 1.0);
}