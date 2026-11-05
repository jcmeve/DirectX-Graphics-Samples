#include "AtmosphereCommon.hlsli"
#include "SkyViewCommon.hlsli"
#include "AtmosphereCloudShadow.hlsli"

Texture2D<float4> TransmittanceLut : register(t0);
Texture2D<float4> MultiScatteringLut : register(t1);
SamplerState LinearClampSampler : register(s0);
RWTexture3D<float4> AtmosphereShadowVolume : register(u0);

[numthreads(8, 8, 1)]
void main( uint3 DTid : SV_DispatchThreadID )
{
    //BSM, view constants, UV, 
    
    uint3 VolumeSize;
    AtmosphereShadowVolume.GetDimensions(VolumeSize.x, VolumeSize.y, VolumeSize.z);
    if (any(DTid.xy >= VolumeSize.xy))
        return;

    float2 UV = (DTid.xy + 0.5) / VolumeSize.xy;
    float3 Dir = GetWorldViewDir(UV);
    
    float3 Origin = CameraPositionKm;
    if (!MoveToTopAtmosphere(Origin, Dir))
    {
        return;
    }
    
    int StepCount = 32;
    float Jitter = InterleavedGradientNoise(UV.xy, CloudShadowFrameIndex);

    float3 Throughput;
    
    Throughput = 1.0;

    float TopDistance = RaySphereFar(Origin, Dir, TopRadius);
    float GroundDistance = RaySphereNear(Origin, Dir, BottomRadius);
    float RayLength = (GroundDistance > 0.0) ? GroundDistance : TopDistance;
    RayLength = min(RayLength, 16);
    
    float CosTheta = dot(Dir, SunDirection);
    float PhaseR = RayleighPhase(CosTheta);
    float PhaseM = MiePhase(CosTheta, MiePhaseG);

    float StepSize = RayLength / StepCount;
    float3 Luminance = 0.0;

    for (int i = 0; i < StepCount; ++i)
    {
        float3 SamplePos = Origin + Dir * ((i + 0.5) * StepSize);
        float r = length(SamplePos);
        OpticalCoefficients Coefficients = GetOpticalCoefficients(r - BottomRadius);

        float SunMu = dot(SamplePos / r, SunDirection); //천정각
        float PlanetShadow = (RaySphereNear(SamplePos, SunDirection, BottomRadius) > 0.0) ? 0.0 : 1.0; //행성과 닿은 경우 태양 가시성 0
        float3 SunTransmittance = SampleTransmittance(TransmittanceLut, LinearClampSampler, r, SunMu) * PlanetShadow;
        {
            float3 PosKm = SamplePos; //CameraPositionKm + Dir * ((i + Jitter) * StepKm);
            float3 PosMeter = float3(PosKm.x, length(PosKm) - BottomRadius, PosKm.z) * 1000.0;
            float CloudShadowTransmittance = GetCloudShadowTransmittance(CloudShadowMap, LinearClampSampler, PosMeter);
            if (CloudShadowTransmittance !=1.0)
            {
                //AtmosphereShadowVolume[float3(DTid.xy, i)] = 1;
                //continue;
                
            }
            SunTransmittance *= 1/(CloudShadowTransmittance);
        }
        
        float3 MultiScattering = SampleMultiScattering(MultiScatteringLut, LinearClampSampler, r, SunMu) * MultiScatteringFactor;
        
        float3 InScattering = SunIlluminance * (
              SunTransmittance * (Coefficients.RayleighScattering * PhaseR + Coefficients.MieScattering * PhaseM));
//            + MultiScattering * (Coefficients.RayleighScattering + Coefficients.MieScattering)); //

        float3 StepTransmittance = exp(-Coefficients.TotalExtinction * StepSize); //구간 투과율
        Luminance += Throughput * (InScattering - InScattering * StepTransmittance) / max(Coefficients.TotalExtinction, 1e-6);
        Throughput *= StepTransmittance;
        
        float Opacity = 1.0 - dot(Throughput, float3(1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0));
        AtmosphereShadowVolume[float3(DTid.xy, i)] = float4(Luminance, Opacity);
    }

    
    
    
    
  


    
}