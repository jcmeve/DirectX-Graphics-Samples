#include "CloudCommon.hlsli"
#include "SKyViewCommon.hlsli"

RWTexture3D<float> CloudLightCache : register(u0);

static const uint kLightStepCount = 10;
static const uint kLightStepStart = 16;



float GetLightOpticalDepth(float3 RelPos, float3 CacheBoundsSize)
{
    float OpticalDepth = 0.0;
    float Distance = 0.0;
    float StepLength = kLightStepStart;
    for (int i = 0; i < kLightStepCount; ++i)
    {
        float3 SampleRelPos = RelPos + SunDirection * (Distance + StepLength);
        float3 Coord = (SampleRelPos - BoundsMin) / CacheBoundsSize;

        float Density = SampleCloudProfile(Coord);
        OpticalDepth += Density * StepLength;

        Distance += StepLength;
        StepLength *= 2.0;
    }
    return OpticalDepth * Extinction;
}


[numthreads(4, 4, 4)]
void main( uint3 DTid : SV_DispatchThreadID )
{
    uint3 CacheSize;
    CloudLightCache.GetDimensions(CacheSize.x, CacheSize.y, CacheSize.z);
    
    float3 CacheBoundsSize = BoundsSize / CacheSize;
    
    if(any(DTid>=CacheSize))
        return;
    
    float3 Position = (float3(DTid) + 0.5) * CacheBoundsSize + BoundsMin;
    
    float OpticalDepth = GetLightOpticalDepth(Position, CacheBoundsSize);
    CloudLightCache[DTid] = OpticalDepth;
}