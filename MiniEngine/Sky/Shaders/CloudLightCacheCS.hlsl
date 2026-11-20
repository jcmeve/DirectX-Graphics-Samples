#include "CloudCommon.hlsli"
#include "CloudShadowCommon.hlsli"

RWTexture3D<float> CloudLightCache : register(u0);

static const uint kLightStepCount = 128;
static const uint kLightStepMeters = 16;



float GetCacheOpticalDepth(float3 LocalPosition, float3 SunDir, OccupancyGrid Grid)
{
    float2 Hit = RayBoxIntersect(LocalPosition, SunDir, 0.0, BoundsSize);
    
    float OpticalDepth = 0.0;
    float t = kLightStepMeters * 0.5;
    
    for (uint i = 0; i < kLightStepCount && t < Hit.y; ++i)
    {
        float3 Position = LocalPosition + t * SunDir;
        
        float SkipDistance = GetOccupancySkipDistance(Position, SunDir, Grid);
        if (SkipDistance > 0.0)
        {
            t += SkipDistance;
            continue;
        }
        
        OpticalDepth += SampleCloudProfile(Position / BoundsSize) * kLightStepMeters;
        t += kLightStepMeters;
    }
    return OpticalDepth * Extinction;
}


[numthreads(4, 4, 4)]
void main( uint3 DTid : SV_DispatchThreadID )
{
    uint3 CacheSize;
    CloudLightCache.GetDimensions(CacheSize.x, CacheSize.y, CacheSize.z);
    if(any(DTid>=CacheSize))
        return;
    
    float3 LocalPosition = (float3(DTid) + 0.5) / float3(CacheSize).xzy * BoundsSize;
    CloudLightCache[DTid] = GetCacheOpticalDepth(LocalPosition, normalize(CloudShadowSunDirection), GetOccupancyGrid());
}