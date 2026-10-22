#include "SkyDebugView.hlsli"

Texture2D<float4> SourceTexture : register(t0);

float4 main(float4 Position : SV_Position, float2 UV : TEXCOORD0) : SV_Target0
{
    return ToOverlayColor(SourceTexture.SampleLevel(LinearClampSampler, UV, 0).rgb);
}