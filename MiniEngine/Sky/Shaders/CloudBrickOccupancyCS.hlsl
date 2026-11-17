#include "CloudCommon.hlsli"
cbuffer OccupancyConstants : register(b5)
{
    uint3 RegionMin;
}

RWTexture3D<float> BrickOccupancy : register(u0);

static const uint kHaloVoxels = kBrickVoxels + 2;
static const uint kHaloCount = kHaloVoxels * kHaloVoxels * kHaloVoxels;
static const uint kGroupThreads = kBrickVoxels * kBrickVoxels * kBrickVoxels;//512

groupshared uint s_MaxDensity;

[numthreads(8, 8, 8)]
void main( uint3 Groupid : SV_GroupID, uint GroupIndex : SV_GroupIndex )
{
    uint3 BrickCount;
    BrickOccupancy.GetDimensions(BrickCount.x, BrickCount.y, BrickCount.z);
    uint3 Brick = RegionMin + Groupid;
    if (any(Brick >= BrickCount))
        return;
    
    if(GroupIndex == 0)
        s_MaxDensity = 0;
    GroupMemoryBarrierWithGroupSync();
    
    int3 VoxelCount;
    CloudModeling.GetDimensions(VoxelCount.x, VoxelCount.y, VoxelCount.z);
    
    float MaxDensity = 0.0;
    
    for (uint i = GroupIndex; i < kHaloCount; i += kGroupThreads)
    {
        int3 Local = int3(i % kHaloVoxels, (i / kHaloVoxels) % kHaloVoxels, i / (kHaloVoxels * kHaloVoxels));
        //int3 Voxel = clamp(int3(Brick * kBrickVoxels) - 1 + Local, 0, VoxelCount - 1);
        int3 Voxel = int3(Brick * kBrickVoxels) - 1 + Local;
        Voxel.xz = (Voxel.xz + VoxelCount.xy) % VoxelCount.xy;
        Voxel.z = clamp(Voxel.z, 0, VoxelCount.z - 1);
        
        float3 Modeling = CloudModeling[Voxel].rgb;
        MaxDensity = max(MaxDensity, Modeling.r * Modeling.b);
    }
    InterlockedMax(s_MaxDensity, asuint(MaxDensity));
    GroupMemoryBarrierWithGroupSync();
    
    if(GroupIndex == 0)
        BrickOccupancy[Brick] = asfloat(s_MaxDensity);

}
