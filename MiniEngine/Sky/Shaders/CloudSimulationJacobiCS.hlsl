#include "CloudSimulationCommon.hlsli"

Texture3D<float> Divergence : register(t7);
Texture3D<float> PrevPressure : register(t8);
RWTexture3D<float> Pressure : register(u0);

float LoadPressure(int3 Cell)
{
    return IsInsideWindow(Cell) ? PrevPressure[Cell] : 0.0;
}

[numthreads(4, 4, 4)]
void main( uint3 DTid : SV_DispatchThreadID )
{
    int3 Cell = int3(DTid);
    float Sum =
    LoadPressure(Cell - int3(1, 0, 0)) + LoadPressure(Cell + int3(1, 0, 0))
    + LoadPressure(Cell - int3(0, 1, 0)) + LoadPressure(Cell + int3(0, 1, 0))
    + LoadPressure(Cell - int3(0, 0, 1)) + LoadPressure(Cell + int3(0, 0, 1));
    
    Pressure[DTid] = (Sum - Divergence[DTid]) / 6.0;
}