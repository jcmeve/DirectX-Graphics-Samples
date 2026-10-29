#include "AtmosphereCloudShadow.hlsli"

Texture3D<float4> AerialPerspectiveVolume : register(t3);
Texture2D<float> SceneDepth : register(t4);
SamplerState LinearClampSampler : register(s0);

float4 main(float4 Position : SV_Position, float2 UV : TEXCOORD0) : SV_Target0
{
    float Depth = SceneDepth[uint2(Position.xy)];

    // Aerial Perspective 
    if (Depth <= 0.0)
        return 0.0;
    
    float DistanceKm = GetSceneViewDistance(UV, Depth) * AerialPerspectiveWorldToKm;
    float4 AerialPerspective = SampleAerialPerspective(AerialPerspectiveVolume, LinearClampSampler, UV, DistanceKm);

    // 구름 그림자 빛줄기 Aerial Perspective에서 뺴서 표현
    float Jitter = InterleavedGradientNoise(Position.xy, CloudShadowFrameIndex);
    AerialPerspective.rgb -= GetCloudShadowedInScattering(AerialPerspectiveVolume, LinearClampSampler, UV, GetWorldViewDir(UV), DistanceKm, Jitter);
    return float4(max(AerialPerspective.rgb, 0.0), AerialPerspective.a);
}