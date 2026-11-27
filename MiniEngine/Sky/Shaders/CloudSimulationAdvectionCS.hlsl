#include "CloudSimulationCommon.hlsli"

Texture3D<float4> CloudDisplacement : register(t15);
Texture3D<float4> Velocity : register(t6);
RWTexture3D<float4> WindowDisplacement : register(u0);
RWTexture3D<uint> ChangeMask : register(u1);

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
    float4 Old = CloudDisplacement[WindowOrigin / kSimulationCellVoxels + DTid];
    WindowDisplacement[DTid] = New;
    ChangeMask[DTid] = any(New != Old) ? 1 : 0;
    //변경 된 경우 마스킹 해 갱신 시 반영
}