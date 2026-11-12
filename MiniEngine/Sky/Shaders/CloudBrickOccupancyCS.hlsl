#include "CloudCommon.hlsli"
cbuffer OccupancyConstants : register(b5)
{
    uint3 RegionMin;
}

RWTexture3D<float> BrickOccupancy : register(u0);

[numthreads(4, 4, 4)]
void main( uint3 DTid : SV_DispatchThreadID )
{
    uint3 BrickCount;
    BrickOccupancy.GetDimensions(BrickCount.x, BrickCount.y, BrickCount.z);
    uint3 Brick = RegionMin + DTid;
    if(any(DTid>=BrickCount))
        return;
    
    uint3 VoxelCount;
    CloudModeling.GetDimensions(VoxelCount.x, VoxelCount.y, VoxelCount.z);
    
    int3 VoxelMin = int3(Brick * kBrickVoxels) - 1;
    int3 VoxelMax = int3((Brick + 1) * kBrickVoxels);
    
    float MaxDensity = 0.0;
    
    for (int z = VoxelMin.z; z <= VoxelMax.z; ++z)
    {
        for (int y = VoxelMin.y; y <= VoxelMax.y; ++y)
        {
            for (int x = VoxelMin.x; x <= VoxelMax.x; ++x)
            {
                int3 Voxel = clamp(int3(x, y, z), 0, int3(VoxelCount) - 1);
                float3 Modeling = CloudModeling.Load(int4(Voxel, 0)).rgb;
                MaxDensity = max(MaxDensity, Modeling.r * Modeling.b);
            }
        }
    }
    BrickOccupancy[Brick] = MaxDensity;

}
