#include "CloudSimulationCommon.hlsli"

Texture3D<float> Divergence : register(t7);
Texture3D<float> Pressure : register(t8);
RWByteAddressBuffer Stats : register(u0);

float LoadPressure(int3 Cell)
{
    return IsInsideWindow(Cell) ? Pressure[Cell] : 0.0;
}

[numthreads(4, 4, 4)]
void main( uint3 DTid : SV_DispatchThreadID )
{
    int3 Cell = int3(DTid);
    float P = Pressure[DTid];
    float Sum =
        LoadPressure(Cell - int3(1, 0, 0)) + LoadPressure(Cell + int3(1, 0, 0))
    + LoadPressure(Cell - int3(0, 1, 0)) + LoadPressure(Cell + int3(0, 1, 0))
    + LoadPressure(Cell - int3(0, 0, 1)) + LoadPressure(Cell + int3(0, 0, 1));
    float Residual = abs(Sum - 6.0 * P - Divergence[DTid]);
    
    uint Dummy;
    Stats.InterlockedMax(0, asuint(Residual), Dummy);
    Stats.InterlockedMax(4, asuint(abs(Divergence[DTid])), Dummy);
    Stats.InterlockedMax(8, asuint(abs(P)), Dummy);

}