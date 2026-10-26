#include "AtmosphereCommon.hlsli"
#include "SkyViewCommon.hlsli"

Texture3D<float4> AerialPerspectiveVolume : register(t3);
Texture2D<float> SceneDepth : register(t4);
SamplerState LinearClampSampler : register(s0);

float4 main(float4 Position : SV_Position, float2 UV : TEXCOORD0) : SV_Target0
{
    float Depth = SceneDepth[uint2(Position.xy)];

    // Aerial Perspective 
    if (Depth <= 0.0)
        return 0.0;

    float2 Ndc = float2(UV.x * 2.0 - 1.0, 1.0 - UV.y * 2.0);
    float4 SurfacePos = mul(InvViewProj, float4(Ndc, Depth, 1.0));
    float4 NearPos = mul(InvViewProj, float4(Ndc, 1.0, 1.0));
    float DistanceKm = length(SurfacePos.xyz / SurfacePos.w - NearPos.xyz / NearPos.w) * AerialPerspectiveWorldToKm;

    //distance->slice
    float Slice = DistanceKm / kAerialPerspectiveMaxDistanceKm * kAerialPerspectiveSliceCount;
    float Weight = 1.0;
    if (Slice < 0.5)//RenderSkyRaymarching.hlsl Âü°í
    {
        Weight = saturate(Slice * 2.0);
        Slice = 0.5;
    }
    float W = sqrt(Slice / kAerialPerspectiveSliceCount);

    return AerialPerspectiveVolume.SampleLevel(LinearClampSampler, float3(UV, W), 0) * Weight;
}