#include "AtmosphereCommon.hlsli"
#include "SkyViewCommon.hlsli"

static const int kSkyViewLutSteps = 64;

Texture2D<float4> TransmittanceLut : register(t0);
Texture2D<float4> MultiScatteringLut : register(t1);
SamplerState LinearClampSampler : register(s0);
RWTexture2D<float4> SkyViewLut : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    uint2 LutSize;
    SkyViewLut.GetDimensions(LutSize.x, LutSize.y);
    if (any(DTid.xy >= LutSize))
        return;

    float2 UV = (DTid.xy + 0.5) / LutSize;

    float ViewHeight = length(CameraPositionKm);
    float ViewZenithCos, LightViewCos;
    UvToSkyViewParams(UV, ViewHeight, ViewZenithCos, LightViewCos);

    // 카메라 기준 로컬 좌표계: 위 = +Y, 태양은 +X 쪽 XY 평면 위
    float SunZenithCos = dot(CameraPositionKm / ViewHeight, SunDirection);
    float3 LocalSunDir = float3(sqrt(saturate(1.0 - SunZenithCos * SunZenithCos)), SunZenithCos, 0.0);

    float ViewZenithSin = sqrt(saturate(1.0 - ViewZenithCos * ViewZenithCos));
    float3 LocalDir = float3(
        ViewZenithSin * LightViewCos,
        ViewZenithCos,
        ViewZenithSin * sqrt(saturate(1.0 - LightViewCos * LightViewCos)));

    float3 Origin = float3(0.0, ViewHeight, 0.0);
    if (!MoveToTopAtmosphere(Origin, LocalDir))
    {
        SkyViewLut[DTid.xy] = float4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    float3 Throughput;
    float3 Luminance = IntegrateScatteredLuminance(TransmittanceLut, MultiScatteringLut, LinearClampSampler,
        Origin, LocalDir, LocalSunDir, SunIlluminance, MultiScatteringFactor, kSkyViewLutSteps, Throughput);
    SkyViewLut[DTid.xy] = float4(Luminance, 1.0);
}