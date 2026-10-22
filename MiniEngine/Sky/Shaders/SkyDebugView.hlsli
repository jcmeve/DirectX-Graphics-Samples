#ifndef SKY_DEBUG_VIEW_HLSLI
#define SKY_DEBUG_VIEW_HLSLI

cbuffer DebugViewConstants : register(b1)
{
    float Exposure;
    float VolumeSlice;
};

SamplerState LinearClampSampler : register(s0);

float4 ToOverlayColor(float3 Color)
{
    return float4(saturate(Color * Exposure), 1.0);
}

#endif 