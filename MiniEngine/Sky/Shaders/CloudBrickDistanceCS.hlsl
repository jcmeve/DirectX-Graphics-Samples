#include "CloudCommon.hlsli"

cbuffer OccupancyConstants : register(b5)
{
    uint3 RegionMin;
}
RWTexture3D<float> BrickOccupancy : register(u0);
RWTexture3D<float> BrickDistance : register(u1);

static const int kMaxBrickDistance = 16;

[numthreads(4, 4, 4)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    uint3 BrickCount;
    BrickDistance.GetDimensions(BrickCount.x, BrickCount.y, BrickCount.z);
    
    uint3 Brick = RegionMin + DTid;
    if (any(Brick >= BrickCount))
        return;
    
    int Nearest = kMaxBrickDistance + 1;
    for (int z = -kMaxBrickDistance; z <= kMaxBrickDistance; ++z)
    {
        for (int y = -kMaxBrickDistance; y <= kMaxBrickDistance; ++y)
        {
            for (int x = -kMaxBrickDistance; x <= kMaxBrickDistance; ++x)
            {
                int3 Other = Brick + int3(x, y, z);
                if (any(Other < 0) || any(Other >= BrickCount))
                    continue;
                if (BrickOccupancy[Other] > 0.0)
                {
                    Nearest = min(Nearest, max(abs(x), max(abs(y), abs(z))));
                }
            }
        }
    }
    BrickDistance[Brick] = Nearest;
    
}