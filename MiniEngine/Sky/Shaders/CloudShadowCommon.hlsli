#ifndef CLOUD_SHADOW_COMMON_HLSLI
#define CLOUD_SHADOW_COMMON_HLSLI

cbuffer CloudShadowConstants : register(b4)
{
    float4x4 CloudShadowWorldToShadow;
    
    float4x4 CloudShadowShadowToWorld;
    
    float3 CloudShadowSunDirection;
    float CloudShadowShaftMaxDistanceKm;

    uint CloudShadowFrameIndex;
    float3 CloudShadowConstantsPad;
};

float GetBeerShadowTransmittance(float3 Bsm, float LightDepth)
{
    return exp(-clamp((LightDepth - Bsm.x) * Bsm.y, 0.0, Bsm.z));
}

// Position->UV,Depth->Åõ°úÀ²
float GetCloudShadowTransmittance(Texture2D<float4> ShadowMap, SamplerState LinearClamp, float3 PositionMeters)
{
    float3 ShadowPos = mul(CloudShadowWorldToShadow, float4(PositionMeters, 1.0)).xyz;
    if (any(ShadowPos.xy < 0.0) || any(ShadowPos.xy > 1.0))
        return 1.0;
    return GetBeerShadowTransmittance(ShadowMap.SampleLevel(LinearClamp, ShadowPos.xy, 0).rgb, ShadowPos.z);
}

#endif