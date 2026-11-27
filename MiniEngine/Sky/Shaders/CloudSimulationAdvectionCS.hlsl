#include "CloudSimulationCommon.hlsli"

Texture3D<float4> CloudDisplacement : register(t15);
Texture3D<float4> Velocity : register(t6);
RWTexture3D<float4> WindowDisplacement : register(u0);
RWTexture3D<uint> ChangeMask : register(u1);
RWTexture3D<float4> CommittedDisplacement : register(u2);

static const float kOccupancyDisplacementThreshold = 0.5;

[numthreads(4, 4, 4)]
void main( uint3 DTid : SV_DispatchThreadID )
{
    uint3 DisplacementCount;
    CloudDisplacement.GetDimensions(DisplacementCount.x, DisplacementCount.y, DisplacementCount.z);
    
    float3 Pos = DTid + 0.5;
    float3 Back = TraceBack(Velocity, Pos, Velocity[DTid].xyz);
    
    float3 GlobalBack = float3(WindowOrigin / kSimulationCellVoxels) + Back;
    float4 Prev = CloudDisplacement.SampleLevel(LinearClampSampler, GlobalBack / float3(DisplacementCount), 0);
    
    float3 D = Prev.xyz + (Pos - Back) * kSimulationCellVoxels;
    float Dilution = Prev.w + GetExpansionSource(CellToVoxel(Pos)) * DeltaTime;

    float4 New = float4(D, Dilution);
    WindowDisplacement[DTid] = New;
    
    uint3 GlobalCell = WindowOrigin / kSimulationCellVoxels + DTid;
    float3 Committed = CommittedDisplacement[GlobalCell].xyz;
    bool bChanged = any(abs(New.xyz - Committed) > kOccupancyDisplacementThreshold);
    ChangeMask[DTid] = bChanged ? 1 : 0;
    if (bChanged)
        CommittedDisplacement[GlobalCell] = New;

}