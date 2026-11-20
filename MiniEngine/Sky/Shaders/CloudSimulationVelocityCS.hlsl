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
    float Temperature = 0.0;
    
    if (!bResetVelocity)
    {
        float3 Back = Pos - PrevVelocity[DTid].xyz * DeltaTime;
        float4 Prev = PrevVelocity.SampleLevel(LinearClampSampler, Back / kSimulationWindowSize, 0);
        
        //이전 지점에서 샘플링
        V = Prev.xyz * exp(-Drag * DeltaTime);
        Temperature = Prev.w * exp(-Cooling * DeltaTime);
    }
    
    if (EventStrength > 0.0)
        Temperature = max(Temperature, EventHeat * GetEventFalloff(WindowOrigin + Pos));
    
    V.z += Buoyancy * Temperature * DeltaTime;
    
    Velocity[DTid] = float4(V, Temperature);
}