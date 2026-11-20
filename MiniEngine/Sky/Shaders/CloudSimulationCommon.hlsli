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
}

static const uint kSimulationWindowSize = 64;
SamplerState LinearClampSampler : register(s0);

bool IsInsideWindow(int3 Cell)
{
    return all(Cell >= 0) && all(Cell < (kSimulationWindowSize));
}

float GetExpansionSource(float3 VoxelPos)
{
    float Falloff = saturate(1.0 - length(VoxelPos - EventCenter) / EventRadius);
    return EventStrength * Falloff * Falloff;
}