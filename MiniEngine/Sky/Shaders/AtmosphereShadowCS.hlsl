#include "AtmosphereCommon.hlsli"
#include "SkyViewCommon.hlsli"
#include "AtmosphereCloudShadow.hlsli"

Texture2D<float4> TransmittanceLut : register(t0);
Texture2D<float4> MultiScatteringLut : register(t1);
SamplerState LinearClampSampler : register(s0);
RWTexture3D<float4> AtmosphereShadowVolume : register(u0);

static const float kLossStepKm = 0.05;
[numthreads(8, 8, 1)]
void main( uint3 DTid : SV_DispatchThreadID )
{
    //BSM, view constants, UV, 
    
    uint3 VolumeSize;
    AtmosphereShadowVolume.GetDimensions(VolumeSize.x, VolumeSize.y, VolumeSize.z);
    if (any(DTid >= VolumeSize))
        return;

    float MaxKm = CloudShadowShaftMaxDistanceKm;
    float3 Origin = CameraPositionKm;
    float3 Dir = GetWorldViewDir((DTid.xy + 0.5) / VolumeSize.xy);

    float RayKm = 0.0;
    if (MaxKm > 0.0 && length(Origin) <= TopRadius)
    {
        float GroundKm = RaySphereNear(Origin, Dir, BottomRadius);
        RayKm = GroundKm > 0.0 ? GroundKm : RaySphereFar(Origin, Dir, TopRadius);
        RayKm = min(RayKm, MaxKm);
    }
    
    float CosTheta = dot(Dir, SunDirection);
    float PhaseR = RayleighPhase(CosTheta);
    float PhaseM = MiePhase(CosTheta, MiePhaseG);
    
    float Jitter = InterleavedGradientNoise(DTid.xy, CloudShadowFrameIndex);
    
    float3 Loss = 0.0;
    float3 Throughput = 1.0;
    float PrevKm = 0.0;
    
    for (uint z = 0; z < VolumeSize.z; ++z)
    {
        float Slice = (z + 0.5) / VolumeSize.z;
        float SliceKm = min(Slice * Slice * MaxKm, RayKm);
        
        float SegmentKm = SliceKm - PrevKm;
        uint SubStepCnt = (uint) ceil(SegmentKm / kLossStepKm);
        float StepSize = SegmentKm / max(SubStepCnt, 1);
        
        for (int i = 0; i < SubStepCnt; ++i)
        {
            float3 SamplePos = Origin + Dir * (PrevKm+( i+Jitter) * StepSize);
            float r = length(SamplePos);
            OpticalCoefficients Coefficients = GetOpticalCoefficients(r - BottomRadius);

            float SunMu = dot(SamplePos / r, SunDirection); //천정각
            float PlanetShadow = (RaySphereNear(SamplePos, SunDirection, BottomRadius) > 0.0) ? 0.0 : 1.0; //행성과 닿은 경우 태양 가시성 0
            float3 SunTransmittance = SampleTransmittance(TransmittanceLut, LinearClampSampler, r, SunMu) * PlanetShadow;

            float3 MultiScattering = SampleMultiScattering(MultiScatteringLut, LinearClampSampler, r, SunMu) * MultiScatteringFactor;
        
            float3 InScattering = SunIlluminance * (
              SunTransmittance * GetCloudOcclusion(LinearClampSampler, SamplePos) * (Coefficients.RayleighScattering * PhaseR + Coefficients.MieScattering * PhaseM)
           );

            float3 StepTransmittance = exp(-Coefficients.TotalExtinction * StepSize); //구간 투과율
            Loss += Throughput * (InScattering - InScattering * StepTransmittance) / max(Coefficients.TotalExtinction, 1e-6);
            Throughput *= StepTransmittance;
        }
        PrevKm = SliceKm;
        AtmosphereShadowVolume[uint3(DTid.xy, z)] = float4(Loss, 0.0);

    }
    
}