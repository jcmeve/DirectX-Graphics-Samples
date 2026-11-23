#include "CloudCommon.hlsli"
#include "CloudShadowCommon.hlsli"

RWTexture3D<float> CloudLightCache : register(u0);

static const uint kLightStepCount = 128;
static const uint kLightStepMeters = 16;



float GetCacheOpticalDepth(float3 LocalPosition, float3 SunDir, OccupancyGrid Grid)
{
    float ExitDistance = (SunDir.y > 1e-5) 
    ? (BoundsSize.y - LocalPosition.y) / SunDir.y 
    : (SunDir.y < -1e-5) 
        ? -LocalPosition.y / SunDir.y 
        : 1e20;
    
    float OpticalDepth = 0.0;
    float t = kLightStepMeters * 0.5;
    
    for (int i = 0; i < kLightStepCount && t < ExitDistance; ++i)
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
    
    float3 LocalPosition = ((float3(DTid) + 0.5) / float3(CacheSize)).xzy * BoundsSize + GlobalWindOffset;
    CloudLightCache[DTid] = GetCacheOpticalDepth(LocalPosition, normalize(CloudShadowSunDirection), GetOccupancyGrid());
}