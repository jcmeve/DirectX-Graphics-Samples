#include "CloudSimulationCommon.hlsli"

Texture3D<float4> WindowDisplacement : register(t6);
RWTexture3D<float4> CloudDisplacement : register(u0);

[numthreads(4, 4, 4)]
void main( uint3 DTid : SV_DispatchThreadID )
{
    CloudDisplacement[WindowOrigin / kSimulationCellVoxels + DTid] = WindowDisplacement[DTid];
}