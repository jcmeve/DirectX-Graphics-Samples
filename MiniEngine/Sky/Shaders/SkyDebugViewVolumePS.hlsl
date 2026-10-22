#include "SkyDebugView.hlsli"

Texture3D<float4> SourceVolume : register(t0);

float4 main(float4 Position : SV_Position, float2 UV : TEXCOORD0) : SV_Target0
{
    return ToOverlayColor(SourceVolume.SampleLevel(LinearClampSampler, float3(UV, VolumeSlice), 0).rgb);
}