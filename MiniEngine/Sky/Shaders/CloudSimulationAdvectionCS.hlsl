#include "CloudSimulationCommon.hlsli"

Texture3D<float4> CloudVolume : register(t5);
Texture3D<float4> Velocity : register(t6);
RWTexture3D<float4> SimulationDensity : register(u0);

[numthreads(4, 4, 4)]
void main( uint3 DTid : SV_DispatchThreadID )
{
    uint3 VoxelCount;
    CloudVolume.GetDimensions(VoxelCount.x, VoxelCount.y, VoxelCount.z);
    
    float3 CellPos = (DTid + 0.5) / kSimulationCellVoxels;
    float3 StartVelocity = Velocity.SampleLevel(LinearClampSampler, CellPos / kSimulationWindowSize, 0).xyz;
    float3 Back = CellToVoxel(TraceBack(Velocity, CellPos, StartVelocity));
    
    float4 Density = CloudVolume.SampleLevel(LinearClampSampler, Back / VoxelCount, 0);
    Density.b *= exp(-GetExpansionSource(CellToVoxel(CellPos)) * DeltaTime);
    SimulationDensity[DTid] = Density;
}