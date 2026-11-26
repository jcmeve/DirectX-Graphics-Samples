#include "CloudSimulationCommon.hlsli"

Texture3D<float> Divergence : register(t7);
RWTexture3D<float> Pressure : register(u0);

cbuffer PoissonConstants : register(b5)
{
    uint Axis;
    uint bSolve;
    uint bFromDivergence;
}

static const float kPi = 3.14159265;
static const float kScale = (2.0 / (kSimulationWindowSize + 1)) * (2.0 / (kSimulationWindowSize + 1)) * (2.0 / (kSimulationWindowSize + 1));

groupshared float Line[kSimulationWindowSize];

uint3 GetCell(uint2 GroupId, uint Index)
{
    return Axis == 0 ? uint3(Index, GroupId.x, GroupId.y)
    : Axis == 1 ? uint3(GroupId.x, Index, GroupId.y)
    : uint3(GroupId.x, GroupId.y, Index);
}

float GetEigen(uint k)
{
    return 2.0 * cos(kPi * (k + 1) / (kSimulationWindowSize + 1)) - 2.0;
}

float TransformLine(uint k)
{
    float A = kPi * (k + 1) / (kSimulationWindowSize + 1);
    float Cos2 = 2.0 * cos(A);
    float S0 = 0.0, S1 = sin(A);
    float Sum = 0.0;
    [unroll]
    for (uint i = 0; i < kSimulationWindowSize; ++i)
    {
        Sum += Line[i] * S1;
        float S2 = Cos2 * S1 - S0;
        S0 = S1;
        S1 = S2;
    }
    return Sum;

}

[numthreads(kSimulationWindowSize, 1, 1)]
void main( uint3 GroupId : SV_GroupID, uint ThreadIndex : SV_GroupIndex)
{
    uint3 Cell = GetCell(GroupId.xy, ThreadIndex);
    if(bFromDivergence)
        Line[ThreadIndex] = Divergence[Cell];
    else
        Line[ThreadIndex] = Pressure[Cell];
    
    GroupMemoryBarrierWithGroupSync();

    float Value = TransformLine(ThreadIndex);
    
    if (bSolve)
    {
        float Eigen = GetEigen(GroupId.x) + GetEigen(GroupId.y) + GetEigen(ThreadIndex);
        GroupMemoryBarrierWithGroupSync();
        Line[ThreadIndex] = Value / Eigen * kScale;
        GroupMemoryBarrierWithGroupSync();
        Value = TransformLine(ThreadIndex);
    }
    Pressure[Cell] = Value;

}