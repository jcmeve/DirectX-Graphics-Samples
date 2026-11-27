#include "CloudSimulationCommon.hlsli"

Texture3D<float> Divergence : register(t7);
RWTexture3D<float> Pressure : register(u0);

cbuffer PoissonConstants : register(b5)
{
    uint Axis;
    uint bSolve;
    uint bFromDivergence;
}

static const uint kLineCells = kSimulationWindowSize; //128
static const uint kSolveCells = kLineCells - 1; //127
static const uint kFFTSize = kLineCells * 2; //256
static const uint kFFTStages = 8; //log2(kFFTSize)

static const float kPi = 3.14159265;
static const float kScale = (2.0 / (kLineCells)) * (2.0 / (kLineCells)) * (2.0 / (kLineCells));

groupshared float2 Data[kFFTSize];
groupshared float2 Twiddle[kLineCells];

uint3 GetCell(uint2 GroupId, uint Index)
{
    return Axis == 0 ? uint3(Index, GroupId.x, GroupId.y)
    : Axis == 1 ? uint3(GroupId.x, Index, GroupId.y)
    : uint3(GroupId.x, GroupId.y, Index);
}

float GetEigen(uint k)
{
    return 2.0 * cos(kPi * (k + 1) / (kLineCells)) - 2.0;
}

uint ReverseIndex(uint Index)
{
    return reversebits(Index) >> (32 - kFFTStages);
}

float2 MulComplex(float2 A, float2 B)
{
    return float2(A.x * B.x - A.y * B.y, A.x * B.y + A.y * B.x);
}

void LoadExtended(float2 Value, uint Thread)
{
    if (Thread < kSolveCells)
    {
        Data[ReverseIndex(Thread + 1)] = Value;
        Data[ReverseIndex(kFFTSize - 1 - Thread)] = -Value;
    }
    else
    {
        Data[ReverseIndex(0)] = 0.0;
        Data[ReverseIndex(kLineCells)] = 0.0;
    }
}

//두 줄(실수부=A, 허수부=B)의 DST-I, Thread 위치 결과 반환
float2 TransformLines(float2 Value, uint Thread)
{
    GroupMemoryBarrierWithGroupSync(); //이전 Data 사용 완료
    LoadExtended(Value, Thread);
    GroupMemoryBarrierWithGroupSync();

    [unroll]
    for (uint Stage = 0; Stage < kFFTStages; ++Stage)
    {
        uint Half = 1u << Stage;
        uint Pos = Thread & (Half - 1);
        uint I0 = ((Thread >> Stage) << (Stage + 1)) + Pos;
        uint I1 = I0 + Half;

        float2 A = Data[I0];
        float2 B = MulComplex(Data[I1], Twiddle[Pos << (kFFTStages - 1 - Stage)]);
        Data[I0] = A + B;
        Data[I1] = A - B;
        GroupMemoryBarrierWithGroupSync();
    }

    //Z = -2i·Sa + 2·Sb
    float2 Z = Data[Thread + 1];
    return (Thread < kSolveCells) ? float2(-Z.y, Z.x) * 0.5 : 0.0;
}

[numthreads(kLineCells, 1, 1)]
void main(uint3 GroupId : SV_GroupID, uint Thread : SV_GroupIndex)
{
    float Angle = -2.0 * kPi * Thread / kFFTSize;
    Twiddle[Thread] = float2(cos(Angle), sin(Angle));

    //그룹당 인접한 두 줄
    uint2 LineA = uint2(GroupId.x * 2, GroupId.y);
    uint2 LineB = LineA + uint2(1, 0);
    uint3 CellA = GetCell(LineA, Thread);
    uint3 CellB = GetCell(LineB, Thread);

    float2 Value;
    if (bFromDivergence)
        Value = float2(Divergence[CellA], Divergence[CellB]);
    else
        Value = float2(Pressure[CellA], Pressure[CellB]);

    Value = TransformLines(Value, Thread);

    if (bSolve)
    {
        float EigenLine = GetEigen(Thread);
        float2 Eigen = float2(GetEigen(LineA.x) + GetEigen(LineA.y), GetEigen(LineB.x) + GetEigen(LineB.y)) + EigenLine;
        Value = Value / Eigen * kScale;
        Value = TransformLines(Value, Thread);
    }

    Pressure[CellA] = Value.x;
    Pressure[CellB] = Value.y;
}