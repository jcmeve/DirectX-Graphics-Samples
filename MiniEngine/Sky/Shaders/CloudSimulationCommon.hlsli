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