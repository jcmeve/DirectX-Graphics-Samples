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
    if (any(DTid >= VolumeSize))
        return;

    float2 UV = (DTid.xy + 0.5) / VolumeSize.xy;
    float3 Dir = GetWorldViewDir(UV);

    float Slice = (DTid.z + 0.5) / VolumeSize.z;
    float MaxDistance = Slice * Slice * kAerialPerspectiveMaxDistanceKm;

    
    float3 Origin = CameraPositionKm;
    if (!MoveToTopAtmosphere(Origin, Dir))
    {
        return;
    }
    //Loss(d) = ∫₀ᵈ    T_view(x)·    σ_s(x)·    p(θ)·    E_sun· T_sun    (x) · (1−    V(x))    dx
    
    
//    {


//// 구름 그림자에 가려진 대기 산란량 = Sigma (구간별 AerialPerspective × (1 - 투과율))
//        float3 GetCloudShadowedInScattering
//        (
//        Texture3D<float4> Volume, SamplerState LinearClamp, float2 UV, float3 Dir, float DistanceKm, float Jitter)
//{
//            float MarchKm = min(DistanceKm, CloudShadowShaftMaxDistanceKm);
//            if (MarchKm <= 0.0)
//                return 0.0;

//            float StepKm = MarchKm / kShaftStepCount;
//            float3 Loss = 0.0;
//            float3 Prev = 0.0;
//            for (uint i = 0; i < kShaftStepCount; ++i)
//            {
//                float3 Next = SampleAerialPerspective(Volume, LinearClamp, UV, (i + 1) * StepKm).rgb;
//                float3 PosKm = CameraPositionKm + Dir * ((i + Jitter) * StepKm);
//                float3 r = float3(PosKm.x, length(PosKm) - BottomRadius, PosKm.z) * 1000.0;
//                Loss += (Next - Prev) * (1.0 - GetCloudShadowTransmittance(CloudShadowMap, LinearClamp, r));
//                Prev = Next;
//            }
//            return Loss;
//        }
//    }
    
    
    
    
    
    
    
    
    
    int StepCount = max(1, (int) (DTid.z + 1) * 2);

    float3 Throughput;
    float3 Luminance = IntegrateScatteredLuminanceLoss(TransmittanceLut, MultiScatteringLut, LinearClampSampler,
        Origin, Dir, SunDirection, SunIlluminance, MultiScatteringFactor, StepCount,
        MaxDistance, true, Throughput);

    float Opacity = 1.0 - dot(Throughput, float3(1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0));
    AtmosphereShadowVolume[DTid] = float4(Luminance, Opacity);
    
}