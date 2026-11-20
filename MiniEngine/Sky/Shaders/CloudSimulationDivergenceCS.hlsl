#include "CloudSimulationCommon.hlsli"

Texture3D<float4> Velocity : register(t6);
RWTexture3D<float> Divergence : register(u0);
RWTexture3D<float> Pressure : register(u1);

float3 LoadVelocity(int3 Cell)
{
    return (IsInsideWindow(Cell) ? Velocity[Cell.xyz].xyz : 0.0);
}

[numthreads(4, 4, 4)]
void main( uint3 DTid : SV_DispatchThreadID )
{
    int3 Cell = int3(DTid);
    float Div = 0.5 * (
    LoadVelocity(Cell + int3(1, 0, 0)).x - LoadVelocity(Cell - int3(1, 0, 0)).x
    + LoadVelocity(Cell + int3(0, 1, 0)).y - LoadVelocity(Cell - int3(0, 1, 0)).y
    + LoadVelocity(Cell + int3(0, 0, 1)).z - LoadVelocity(Cell - int3(0, 0, 1)).z
    );
    
    Div -= GetExpansionSource(float3(WindowOrigin + DTid) + 0.5);
    
    Divergence[DTid] = Div;
    
    if(bResetVelocity)
        Pressure[DTid] = 0.0;
}