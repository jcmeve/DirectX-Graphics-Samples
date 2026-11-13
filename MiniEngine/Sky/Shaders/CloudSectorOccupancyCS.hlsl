#include "CloudCommon.hlsli"

cbuffer OccupancyConstants : register(b5)
{
    uint3 RegionMin;
}
RWTexture3D<float> BrickOccupancy : register(u0);
RWTexture3D<float> SectorOccupancy : register(u1);

[numthreads(4, 4, 4)]
void main( uint3 DTid : SV_DispatchThreadID )
{
    uint3 SectorCount;
    SectorOccupancy.GetDimensions(SectorCount.x, SectorCount.y, SectorCount.z);
    
    uint3 Sector = RegionMin + DTid;
    if(any(Sector>=SectorCount))
        return;
    
    
    uint3 BrickCount;
    BrickOccupancy.GetDimensions(BrickCount.x, BrickCount.y, BrickCount.z);
    uint3 BrickMin = Sector + kSectorBricks;
    uint3 BrickMax = min(BrickMin + kSectorBricks, BrickCount);
    
    float MaxDensity = 0.0;
    for (int z = BrickMin.z; z < BrickMax.z; ++z)
    {
        for (int y = BrickMin.y; y < BrickMax.y; ++y)
        {
            for (int x = BrickMin.x; x < BrickMax.x; ++x)
            {
                MaxDensity = max(MaxDensity, BrickOccupancy[uint3(x, y, z)]);
            }
        }
    }
    
    SectorOccupancy[Sector] = MaxDensity;
    
}
