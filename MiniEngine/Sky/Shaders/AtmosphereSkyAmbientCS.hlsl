#include "AtmosphereCommon.hlsli"
#include "SkyViewCommon.hlsli"

static const uint kZenithSampleCount = 16;
static const uint kAzimuthSampleCount = 16;

Texture2D<float4> SkyViewLut : register(t0);
SamplerState LinearClampSampler : register(s0);
RWTexture2D<float4> SkyAmbient : register(u0);

[numthreads(1, 1, 1)]
void main()
{
    float ViewHeight = length(CameraPositionKm);
    float DeltaZenith = 0.5 * kPi / kZenithSampleCount;
    float DeltaAzimuth = kPi / kAzimuthSampleCount;

    float3 Irradiance = 0.0;
    for (uint i = 0; i < kZenithSampleCount; ++i)
    {
        float Zenith = (i + 0.5) * DeltaZenith;
        float CosZenith = cos(Zenith);
        float SinZenith = sin(Zenith);
        for (uint j = 0; j < kAzimuthSampleCount; ++j)
        {
            float CosAzimuth = cos((j + 0.5) * DeltaAzimuth);
            float2 UV = SkyViewParamsToUv(false, CosZenith, CosAzimuth, ViewHeight);
            float3 Radiance = SkyViewLut.SampleLevel(LinearClampSampler, UV, 0).rgb;
            Irradiance += Radiance * CosZenith * SinZenith;
        }
    }
    Irradiance *= 2.0 * DeltaZenith * DeltaAzimuth;
    SkyAmbient[uint2(0, 0)] = float4(Irradiance, 1.0);
}