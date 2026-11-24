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
    uint EventType;
    
    float3 EventDirection;
    float EventSpeed;
    
    float EventHalfLength;
    float EventInjection;
    float2 SimulationPad;
}

static const uint kEventExplosion = 0;
static const uint kEventWind = 1;
static const uint kEventVortex = 2;
static const float kEventRelaxRate = 8.0;


static const uint kSimulationWindowSize = 64;
static const uint kSimulationCellVoxels = 2; //시뮬1셀 = 2복셀
static const uint kSimulationWindowVoxels = kSimulationWindowSize * kSimulationCellVoxels;
static const float kSpongeCells = 4.0;
static const float kSpongeRate = 8.0;

static const float kVortexCoreRatio = 0.25;
static const float kVortexInFlow = 0.3;

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
    float3 Offset = VoxelPos - EventCenter;
    float AxisDistance = clamp(dot(Offset, EventDirection), -EventHalfLength, EventHalfLength);
    float Falloff = saturate(1.0 - length(Offset - AxisDistance * EventDirection) / EventRadius);
    return Falloff * Falloff;
}

float GetExpansionSource(float3 VoxelPos)
{
    return EventStrength * EventInjection * GetEventFalloff(VoxelPos);
}

float3 GetEventVelocity(float3 VoxelPos)
{
    if(EventType == kEventWind)
        return EventDirection * EventSpeed;

    float3 Offset = VoxelPos - EventCenter;
    float3 R = Offset - dot(Offset, EventDirection) * EventDirection;
    float Distance = length(R);
    if (Distance < 1e-3)
        return 0.0;
    float Core = EventRadius * kVortexCoreRatio;
    float Tangential = EventSpeed * (Distance < Core ? Distance / Core : Core / Distance);
    float3 RDir = R / Distance;
    return Tangential * (cross(EventDirection, RDir) - kVortexInFlow * RDir);
}

bool IsOpenBottom()
{
    return WindowOrigin.z == 0;
}

float3 LoadWindowVelocity(Texture3D<float4> Velocity, int3 Cell)
{
    return Velocity[clamp(Cell, 0, int(kSimulationWindowSize) - 1)].xyz;
}
