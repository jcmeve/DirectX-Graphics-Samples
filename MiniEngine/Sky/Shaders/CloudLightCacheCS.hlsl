#include "CloudCommon.hlsli"
#include "CloudShadowCommon.hlsli"

cbuffer LightCacheConstants : register(b5)
{
    uint SliceStride;
    uint SlicePhase;
};

RWTexture3D<float> CloudLightCache : register(u0);

static const uint kLightStepCount = 128;
static const uint kLightStepMeters = 16;
static const float kMaxOpticalDepth = 20.0;

float GetCacheOpticalDepth(float3 LocalPosition, float3 SunDir, OccupancyGrid Grid)
{
    
    float2 Hit = RayBoxIntersect(LocalPosition, SunDir, 0.0, BoundsSize);
    
    float OpticalDepth = 0.0;
    float t = kLightStepMeters * 0.5;
    
    for (int i = 0; i < kLightStepCount && t < Hit.y; ++i)
    {
        float3 Position = LocalPosition + t * SunDir;
        Position.xz -= floor(Position.xz / BoundsSize.xz) * BoundsSize.xz;
        
        float SkipDistance = GetOccupancySkipDistance(Position, SunDir, Grid);
        if (SkipDistance > 0.0)
        {
            t += SkipDistance;
            continue;
        }
        
        OpticalDepth += SampleCloudProfile(Position / BoundsSize) * kLightStepMeters;
        if (OpticalDepth * Extinction > kMaxOpticalDepth)
            break;
        t += kLightStepMeters;
    }
    return OpticalDepth * Extinction;
}


[numthreads(4, 4, 4)]
void main( uint3 DTid : SV_DispatchThreadID )
{
    uint3 CacheSize;
    CloudLightCache.GetDimensions(CacheSize.x, CacheSize.y, CacheSize.z);

    uint3 Cell = uint3(DTid.xy, DTid.z * SliceStride + SlicePhase);
    if (any(Cell >= CacheSize))
        return;
    
    float3 LocalPosition = ((float3(Cell) + 0.5) / float3(CacheSize)).xzy * BoundsSize + GlobalWindOffset;
    LocalPosition.xz -= floor(LocalPosition.xz / BoundsSize.xz) * BoundsSize.xz;
    CloudLightCache[Cell] = GetCacheOpticalDepth(LocalPosition, normalize(CloudShadowSunDirection), GetOccupancyGrid());
}