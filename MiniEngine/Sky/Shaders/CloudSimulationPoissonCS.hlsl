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
static const uint kFFTStages = 4; //log4(256)
static const uint kFFTThreads = kFFTSize / 4; //stage당 radix-4 butterfly 수
static const uint kGroupLines = 16; //그룹당 줄 수 (x 연속 접근용)
static const uint kGroupFFTs = kGroupLines / 2; //두 줄을 실수/허수부로 묶음
static const uint kGroupThreads = kGroupFFTs * kFFTThreads; //512
static const uint kValuesPerThread = kGroupLines * kLineCells / kGroupThreads; //4
static const float kPi = 3.14159265;
static const float kScale = (2.0 / kLineCells) * (2.0 / kLineCells) * (2.0 / kLineCells);

static const uint kLineStride = kFFTSize + kFFTSize / 32 + 1; //265, bank 충돌 방지 padding

groupshared float Data[kGroupLines * kLineStride];
groupshared float2 Twiddle[kFFTSize];

//32개마다 1칸 padding
uint GetDataIndex(uint Line, uint Index)
{
    return Line * kLineStride + Index + (Index >> 5);
}


struct LineCell
{
    uint Line; //그룹 내 줄
    uint Along; //줄 방향 셀
};

//연속 메모리 방향이 스레드 순서와 맞도록 배치
LineCell GetLineCell(uint Flat)
{
    LineCell Ret;
    if (Axis == 0)
    {
        Ret.Line = Flat / kLineCells;
        Ret.Along = Flat % kLineCells;
    }
    else
    {
        Ret.Line = Flat % kGroupLines;
        Ret.Along = Flat / kGroupLines;
    }
    return Ret;
}

uint3 GetCell(uint2 LineId, uint Index)
{
    return Axis == 0 ? uint3(Index, LineId.x, LineId.y)
    : Axis == 1 ? uint3(LineId.x, Index, LineId.y)
    : uint3(LineId.x, LineId.y, Index);
}

float GetEigen(uint k)
{
    return 2.0 * cos(kPi * (k + 1) / kLineCells) - 2.0;
}

//8비트 4진 자릿수 역순
uint ReverseDigits(uint Index)
{
    return ((Index & 3) << 6) | ((Index & 12) << 2) | ((Index >> 2) & 12) | (Index >> 6);
}

float2 MulComplex(float2 A, float2 B)
{
    return float2(A.x * B.x - A.y * B.y, A.x * B.y + A.y * B.x);
}

//홀대칭 확장 x[0]=x[128]=0, x[n+1]=a[n], x[255-n]=-a[n]
void StoreExtended(LineCell C, float Value)
{
    if (C.Along < kSolveCells)
    {
        Data[GetDataIndex(C.Line, ReverseDigits(C.Along + 1))] = Value;
        Data[GetDataIndex(C.Line, ReverseDigits(kFFTSize - 1 - C.Along))] = -Value;
    }
    else
    {
        Data[GetDataIndex(C.Line, ReverseDigits(0))] = 0.0;
        Data[GetDataIndex(C.Line, ReverseDigits(kLineCells))] = 0.0;
    }
}

//Z = -2i·Sa + 2·Sb
float LoadResult(LineCell C)
{
    if (C.Along >= kSolveCells)
        return 0.0;
    uint RealLine = C.Line & ~1u;
    float Re = Data[GetDataIndex(RealLine, C.Along + 1)];
    float Im = Data[GetDataIndex(RealLine + 1, C.Along + 1)];
    return (C.Line & 1) ? Re * 0.5 : -Im * 0.5;
}

void RunFFT(uint Thread)
{
    uint RealBase = (Thread / kFFTThreads) * 2 * kFFTSize;
    uint ImagBase = RealBase + kFFTSize;
    uint Butterfly = Thread % kFFTThreads;
    uint RealLine = (Thread / kFFTThreads) * 2;
    [unroll]
    for (uint Stage = 0; Stage < kFFTStages; ++Stage)
    {
        uint Span = 1u << (2 * Stage);
        uint Pos = Butterfly & (Span - 1);
        uint Base = (Butterfly >> (2 * Stage)) * Span * 4 + Pos;
        uint TwiddleStep = Pos * (kFFTSize / (4 * Span));

        float2 A[4];
        [unroll]
        for (uint m = 0; m < 4; ++m)
        {
            uint I = Base + m * Span;
            A[m] = MulComplex(float2(Data[GetDataIndex(RealLine, I)], Data[GetDataIndex(RealLine + 1, I)]), Twiddle[TwiddleStep * m]);
        }

        float2 Sum02 = A[0] + A[2];
        float2 Diff02 = A[0] - A[2];
        float2 Sum13 = A[1] + A[3];
        float2 Diff13 = A[1] - A[3];
        float2 MinusIDiff13 = float2(Diff13.y, -Diff13.x); //-i * Diff13

        float2 Y[4] = { Sum02 + Sum13, Diff02 + MinusIDiff13, Sum02 - Sum13, Diff02 - MinusIDiff13 };
        [unroll]
        for (uint n = 0; n < 4; ++n)
        {
            uint I = Base + n * Span;
            Data[GetDataIndex(RealLine, I)] = Y[n].x;
            Data[GetDataIndex(RealLine + 1, I)] = Y[n].y;
        }
        GroupMemoryBarrierWithGroupSync();
    }
}

void TransformValues(inout float Values[kValuesPerThread], uint Thread)
{
    GroupMemoryBarrierWithGroupSync(); //이전 Data 사용 완료
    [unroll]
    for (uint k = 0; k < kValuesPerThread; ++k)
        StoreExtended(GetLineCell(Thread + k * kGroupThreads), Values[k]);
    GroupMemoryBarrierWithGroupSync();

    RunFFT(Thread);

    [unroll]
    for (uint j = 0; j < kValuesPerThread; ++j)
        Values[j] = LoadResult(GetLineCell(Thread + j * kGroupThreads));
}

[numthreads(kGroupThreads, 1, 1)]
void main(uint3 GroupId : SV_GroupID, uint Thread : SV_GroupIndex)
{
    if (Thread < kFFTSize)
    {
        float Angle = -2.0 * kPi * Thread / kFFTSize;
        Twiddle[Thread] = float2(cos(Angle), sin(Angle));
    }

    uint LineBase = GroupId.x * kGroupLines;
    float Values[kValuesPerThread];
    [unroll]
    for (uint k = 0; k < kValuesPerThread; ++k)
    {
        LineCell C = GetLineCell(Thread + k * kGroupThreads);
        uint3 Cell = GetCell(uint2(LineBase + C.Line, GroupId.y), C.Along);
        if (bFromDivergence)
            Values[k] = Divergence[Cell];
        else
            Values[k] = Pressure[Cell];
    }

    TransformValues(Values, Thread);

    if (bSolve)
    {
        [unroll]
        for (uint k = 0; k < kValuesPerThread; ++k)
        {
            LineCell C = GetLineCell(Thread + k * kGroupThreads);
            float Eigen = GetEigen(LineBase + C.Line) + GetEigen(GroupId.y) + GetEigen(C.Along);
            Values[k] = Values[k] / Eigen * kScale;
        }
        TransformValues(Values, Thread);
    }

    [unroll]
    for (uint k = 0; k < kValuesPerThread; ++k)
    {
        LineCell C = GetLineCell(Thread + k * kGroupThreads);
        Pressure[GetCell(uint2(LineBase + C.Line, GroupId.y), C.Along)] = Values[k];
    }
}