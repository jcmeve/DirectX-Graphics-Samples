#include "CloudCommon.hlsli"
#include "CloudShadowCommon.hlsli"

RWTexture2D<float4> CloudShadowMap : register(u0);

static const float kShadowStepMeters = 16.0;
static const uint kShadowMaxSteps = 256;
static const float kNoiseTexelMeters = 1.0 / (kNoiseFrequency * 128.0); // 노이즈 1텍셀 = 0.78m

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    uint2 Size;
    CloudShadowMap.GetDimensions(Size.x, Size.y);
    if (any(DTid.xy >= Size))
        return;

    // UV->WorldPosition
    float2 UV = (DTid.xy + 0.5) / Size;
    float3 RayOrigin = mul(CloudShadowShadowToWorld, float4(UV, 0.0, 1.0)).xyz;
    float3 RayDir = -CloudShadowSunDirection;

    //한 텍셀이 표현하는 공간 크기 계산
    float TexelMeters = max(length(mul(CloudShadowShadowToWorld, float4(1.0 / Size.x, 0.0, 0.0, 0.0)).xyz),
                            length(mul(CloudShadowShadowToWorld, float4(0.0, 1.0 / Size.y, 0.0, 0.0)).xyz));
    // 텍셀이 표현하는 공간이 크다면 밉 레벨 올림
    float MipLevel = log2(max(TexelMeters / kNoiseTexelMeters, 1.0));

    float3 BoxMin = BoundsMin + CameraPosition;
    float2 Hit = RayBoxIntersect(RayOrigin, RayDir, BoxMin, BoxMin + BoundsSize);

    float OpticalDepth = 0.0;
    float FrontDepth = max(Hit.y, 0.0);
    float BackDepth = FrontDepth;
    float t = Hit.x + kShadowStepMeters * 0.5;
    for (uint i = 0; i < kShadowMaxSteps && t < Hit.y; ++i)
    {
        float3 WorldPos = RayOrigin + RayDir * t;
        float3 Coord = (WorldPos - BoxMin) / BoundsSize;

        // 빈 공간은 SDF로 건너뜀
        float Sdf = SampleCloudSdf(Coord);
        if (Sdf > kShadowStepMeters)
        {
            t += Sdf;
            continue;
        }

        float Density = GetVoxelCloudShadowDensity(Coord, WorldPos, MipLevel);
        if (Density > 0.0)
        {
            //최초 Depth갱신
            if (OpticalDepth <= 0.0)
                FrontDepth = t - kShadowStepMeters * 0.5;
            //가장 마지막에 닿았던 Depth 갱신
            BackDepth = t + kShadowStepMeters * 0.5;
            OpticalDepth += Density * kShadowStepMeters;
        }
        t += kShadowStepMeters;
    }
    OpticalDepth *= Extinction;

    // 평균 소멸계수 = 광학 깊이 / 두께
    float MeanExtinction = (OpticalDepth > 0.0) ? OpticalDepth / (BackDepth - FrontDepth) : 0.0;
    CloudShadowMap[DTid.xy] = float4(FrontDepth, MeanExtinction, OpticalDepth, 0.0);
}