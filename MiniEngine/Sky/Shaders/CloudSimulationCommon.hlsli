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
    float Buoyancy;//부력
    float Cooling;
    float SimulationPad;
}

static const uint kSimulationWindowSize = 64;
static const uint kSimulationCellVoxels = 2; //시뮬1셀 = 2복셀
static const uint kSimulationWindowVoxels = kSimulationWindowSize * kSimulationCellVoxels;
static const float kSpongeCells = 4.0;
static const float kSpongeRate = 8.0;

SamplerState LinearClampSampler : register(s0);

bool IsInsideWindow(int3 Cell)
{
    return all(Cell >= 0) && all(Cell < int(kSimulationWindowSize));
}

float3 CellToVoxel(float3 CellPos)
{
    return float3(WindowOrigin) + CellPos * kSimulationCellVoxels;
}

float3 TraceBack(Texture3D<float4> Velocity, float3 CellPos, float3 StartVelocity)
{
    float3 Mid = CellPos - 0.5 * DeltaTime * StartVelocity;
    float3 MidVelocity = Velocity.SampleLevel(LinearClampSampler, Mid / kSimulationWindowSize, 0).xyz;
    return CellPos - DeltaTime * MidVelocity;
}

float GetEventFalloff(float3 VoxelPos)
{
    float Falloff = saturate(1.0 - length(VoxelPos - EventCenter) / EventRadius);
    return Falloff * Falloff;
}

float GetExpansionSource(float3 VoxelPos)
{
    return EventStrength * GetEventFalloff(VoxelPos);
}