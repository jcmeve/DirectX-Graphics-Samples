#include "CloudSimulationCommon.hlsli"

Texture3D<float4> PrevVelocity : register(t6);
RWTexture3D<float4> Velocity : register(u0);

[numthreads(4, 4, 4)]
void main( uint3 DTid : SV_DispatchThreadID )
{
    if (any(DTid == 0) || any(DTid == kSimulationWindowSize-1))
    {
        Velocity[DTid] = 0.0;
        return;
    }
    
    float3 Pos = DTid + 0.5;
    float3 V = 0.0;
    if (!bResetVelocity)
    {
        float3 Back = Pos - PrevVelocity[DTid].xyz * DeltaTime;
        
        //이전 지점에서 샘플링
        V = PrevVelocity.SampleLevel(LinearClampSampler, Back / kSimulationWindowSize, 0).xyz;
        V *= exp(-Drag * DeltaTime);
    }
    
    Velocity[DTid] = float4(V, 0.0);
}