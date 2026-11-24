#include "CloudSimulationCommon.hlsli"

Texture3D<float> Pressure : register(t8);
RWTexture3D<float4> Velocity : register(u0);

float LoadPressure(int3 Cell)
{
    return IsInsideWindow(Cell) ? Pressure[Cell] : 0.0;
}

[numthreads(4, 4, 4)]
void main( uint3 DTid : SV_DispatchThreadID )
{
    int3 Cell = int3(DTid);
    
    float3 Gradient = 0.5 * float3(
        LoadPressure(Cell + int3(1, 0, 0)) - LoadPressure(Cell - int3(1, 0, 0))
    , LoadPressure(Cell + int3(0, 1, 0)) - LoadPressure(Cell - int3(0, 1, 0))
    , LoadPressure(Cell + int3(0, 0, 1)) - LoadPressure(Cell - int3(0, 0, 1))
    );
    
    float4 V = Velocity[DTid];
    Velocity[DTid] = float4(V.xyz - Gradient, V.w);

}