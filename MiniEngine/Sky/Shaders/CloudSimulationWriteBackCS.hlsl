#include "CloudSimulationCommon.hlsli"

Texture3D<float4> SimulationDensity : register(t6);
RWTexture3D<float4> CloudVolume : register(u0);

[numthreads(4, 4, 4)]
void main( uint3 DTid : SV_DispatchThreadID )
{
    CloudVolume[WindowOrigin + DTid] = SimulationDensity[DTid];
}