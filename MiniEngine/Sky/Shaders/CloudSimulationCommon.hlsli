cbuffer SimulationConstants : register(b6)
{
    uint3 WindowOrigin;
    float DeltaTime;
 
    float3 EventCenter;
    float EventRadius;
    
    float EventStrength;
    float Drag;
    uint bEventActive;
    uint bResetVelocity;
    
    float EventHeat;
    float Buoyancy;//บฮทย
    float Cooling;
    float SimulationPad;
}

static const uint kSimulationWindowSize = 64;
SamplerState LinearClampSampler : register(s0);

bool IsInsideWindow(int3 Cell)
{
    return all(Cell >= 0) && all(Cell < (kSimulationWindowSize));
}
float GetEventFalloff(float3 VoxelPos)
{
    float Falloff = saturate(1.0 - length(VoxelPos - EventCenter) / EventRadius);
    return Falloff * Falloff;
}
float GetExpansionSource(float3 VoxelPos)
{
    float Falloff = saturate(1.0 - length(VoxelPos - EventCenter) / EventRadius);
    return EventStrength * GetEventFalloff(VoxelPos);
}