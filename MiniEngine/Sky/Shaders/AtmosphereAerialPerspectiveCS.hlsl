#include "AtmosphereCommon.hlsli"
#include "SkyViewCommon.hlsli"

Texture2D<float4> TransmittanceLut : register(t0);
Texture2D<float4> MultiScatteringLut : register(t1);
SamplerState LinearClampSampler : register(s0);
RWTexture3D<float4> AerialPerspectiveVolume : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    uint3 VolumeSize;
    AerialPerspectiveVolume.GetDimensions(VolumeSize.x, VolumeSize.y, VolumeSize.z);
    if (any(DTid >= VolumeSize))
        return;

    float2 UV = (DTid.xy + 0.5) / VolumeSize.xy;
    float3 Dir = GetWorldViewDir(UV);

    float Slice = (DTid.z + 0.5) / VolumeSize.z;
    float MaxDistance = Slice * Slice * kAerialPerspectiveMaxDistanceKm;

    
    float3 Origin = CameraPositionKm;
    if (!MoveToTopAtmosphere(Origin, Dir))
    {
        AerialPerspectiveVolume[DTid] = 0.0;
        return;
    }
    MaxDistance -= length(Origin - CameraPositionKm);
    if (MaxDistance <= 0.0)
    {
        AerialPerspectiveVolume[DTid] = 0.0;
        return;
    }

    int StepCount = max(1, (int) (DTid.z + 1) * 2);

    float3 Throughput;
    float3 Luminance = IntegrateScatteredLuminance(TransmittanceLut, MultiScatteringLut, LinearClampSampler,//기존과 같은 패스, 제한된 거리만 적분
        Origin, Dir, SunDirection, SunIlluminance, MultiScatteringFactor, StepCount,
        MaxDistance, true, Throughput);

    float Opacity = 1.0 - dot(Throughput, float3(1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0));
    AerialPerspectiveVolume[DTid] = float4(Luminance, Opacity);
}