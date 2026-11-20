#include "CloudSimulationCommon.hlsli"

Texture3D<float4> CloudVolume : register(t5);
Texture3D<float4> Velocity : register(t6);
RWTexture3D<float4> SimulationDensity : register(u0);

[numthreads(4, 4, 4)]
void main( uint3 DTid : SV_DispatchThreadID )
{
    uint3 VoxelCount;
    CloudVolume.GetDimensions(VoxelCount.x, VoxelCount.y, VoxelCount.z);
    
    float3 Pos = WindowOrigin + DTid + 0.5;
    float3 Back = Pos - Velocity[DTid].xyz * DeltaTime;
    float4 Density = CloudVolume.SampleLevel(LinearClampSampler, Back / VoxelCount, 0);
    Density.r *= exp(-GetExpansionSource(Pos) * DeltaTime);
    SimulationDensity[DTid] = Density;
}