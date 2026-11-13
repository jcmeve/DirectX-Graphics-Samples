#ifndef CLOUD_COMMON_HLSLI
#define CLOUD_COMMON_HLSLI

cbuffer CloudConstants : register(b3)
{
    float3 BoundsMin; //m
    float Extinction; //소멸 계수 (1/m)

    float3 BoundsSize; //m
    float StepScale; // 기본 스텝 = max(1, sqrt(거리) * StepScale)

    float3 CameraPosition; //m
    float WorldToMeters;

    float AmbientScale;
    float MipDistanceScale;
    uint MaxSteps;
    uint FrameIndex;
};

Texture3D<float4> CloudNoise : register(t4); // RG = Curly-Alligator 저/고주파, BA = Alligator 저/고주파
Texture3D<float4> CloudModeling : register(t5); // R = Dimensional Profile, G = Detail Type, B = Density Scale
Texture3D<float4> CloudField : register(t6); // R = SDF

Texture3D<float> CloudBrickOccupancy : register(t12);
Texture3D<float> CloudSectorOccupancy : register(t13);
static const uint kBrickVoxels = 8;
static const uint kSectorBricks = 4;
static const float kCellExitBias = 0.05;

struct OccupancyGrid
{
    float3 BrickMeters;
    float3 SectorMeters;
    int3 BrickCount;
};

OccupancyGrid GetOccupancyGrid()
{
    uint3 VoxelCount;
    CloudModeling.GetDimensions(VoxelCount.x, VoxelCount.y, VoxelCount.z);
    
    uint3 BrickCount;
    CloudBrickOccupancy.GetDimensions(BrickCount.x, BrickCount.y, BrickCount.z);
    
    OccupancyGrid Ret;
    Ret.BrickMeters = BoundsSize / float3(VoxelCount.xzy) * kBrickVoxels;
    Ret.SectorMeters = Ret.BrickMeters * kSectorBricks;
    Ret.BrickCount = int3(BrickCount.xzy);

    return Ret;
}

float GetCellExitDistance(float3 LocalPosition, float3 Dir, float3 CellMeters)
{
    float3 CellMin = floor(LocalPosition / CellMeters) * CellMeters;
    float3 Boundary = CellMin + float3((Dir.x > 0.0 ? CellMeters.x : 0.0), (Dir.y > 0.0 ? CellMeters.y : 0.0), (Dir.z > 0.0 ? CellMeters.z : 0.0));
    float3 Distance = float3((abs(Dir.x) > 1e-5) ? ((Boundary.x - LocalPosition.x) / Dir.x) : 1e20,
                             (abs(Dir.y) > 1e-5) ? ((Boundary.y - LocalPosition.y) / Dir.y) : 1e20,
                             (abs(Dir.z) > 1e-5) ? ((Boundary.z - LocalPosition.z) / Dir.z) : 1e20);
    return max(min(min(Distance.x, Distance.y), Distance.z), 0.0);
}

float GetOccupancySkipDistance(float3 LocalPosition, float3 Dir, OccupancyGrid Grid)
{
    int3 Brick = clamp(int3(floor(LocalPosition / Grid.BrickMeters)), 0, Grid.BrickCount - 1);
    int3 Sector = Brick / int(kSectorBricks);

    //빈 영역 검사
    if (CloudSectorOccupancy[Sector.xzy] <= 0.0)
    {
        return GetCellExitDistance(LocalPosition, Dir, Grid.SectorMeters) + kCellExitBias;
    }
    if(CloudBrickOccupancy[Brick.xzy] <= 0.0){
        return GetCellExitDistance(LocalPosition, Dir, Grid.BrickMeters) + kCellExitBias;
    }
    return 0.0;
}


SamplerState LinearClampSampler : register(s0);
SamplerState LinearWrapSampler : register(s1);

static const float kSdfMinMeters = -256.0;
static const float kSdfMaxMeters = 4096.0;
static const float kNoiseFrequency = 0.01; // 노이즈 100m 타일
static const float kHFDetailDistance = 150.0;
static const float kNoiseTexelMeters = 1.0 / (kNoiseFrequency * 128.0);
static const float kCloudEpsilon = 1e-4;
static const uint kAmortizationSize = 4; //한 축

static const uint Bayer4x4[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };

uint2 GetAmortizationOffset(uint FrameIndex)
{
    uint Index = FrameIndex % (kAmortizationSize * kAmortizationSize);
    return uint2(Bayer4x4[Index] % kAmortizationSize, Bayer4x4[Index] / kAmortizationSize);
    //return uint2(Index % kAmortizationSize, Index / kAmortizationSize);
}

float GetNoiseMipFromFootprint(float FootprintMeters)
{
    return log2(max(FootprintMeters / kNoiseTexelMeters, 1.0));
}

float ValueRemap(float Value, float OldMin, float OldMax, float NewMin, float NewMax)
{
    float Normalized = saturate((Value - OldMin) / (OldMax - OldMin));
    return NewMin + Normalized * (NewMax - NewMin);
}

float ValueErosion(float Value, float OldMin)
{
    return saturate((Value - OldMin) / (1.0 - OldMin));
}

float GetFractionFromValue(float Value, float Min, float Max)
{
    return saturate((Value - Min) / (Max - Min));
}

// 월드xyz->텍스쳐xzy
float3 ToVolumeUVW(float3 Coord)
{
    return Coord.xzy;
}

bool IsInsideBounds(float3 Coord)
{
    return all(Coord >= 0.0) && all(Coord <= 1.0);
}

float SampleCloudSdf(float3 Coord)
{
    float Encoded = CloudField.SampleLevel(LinearClampSampler, ToVolumeUVW(Coord), 0).r;
    return lerp(kSdfMinMeters, kSdfMaxMeters, Encoded);
}

float SampleCloudProfile(float3 Coord)
{
    if (!IsInsideBounds(Coord))
        return 0.0;

    float3 Modeling = CloudModeling.SampleLevel(LinearClampSampler, ToVolumeUVW(Coord), 0).rgb;
    return Modeling.r * Modeling.b;
}

float GetUprezzedVoxelCloudDensity(float3 SamplePosition, float Distance, float MipLevel, float Profile, float Type, float DensityScale, bool bHFDetails)
{
    float4 Noise = CloudNoise.SampleLevel(LinearWrapSampler, SamplePosition.xzy * kNoiseFrequency, MipLevel);

    float WispyNoise = lerp(Noise.r, Noise.g, Profile);

    float BillowyTypeGradient = pow(Profile, 0.25);
    float BillowyNoise = lerp(Noise.b * 0.3, Noise.a * 0.3, BillowyTypeGradient);

    // Type 0 = wispy, 1 = billowy
    float NoiseComposite = lerp(WispyNoise, BillowyNoise, Type);
    if (bHFDetails)
    {
        float HHFNoise = saturate(lerp(1.0 - pow(abs(abs(Noise.g * 2.0 - 1.0) * 2.0 - 1.0), 4.0),
            pow(abs(abs(Noise.a * 2.0 - 1.0) * 2.0 - 1.0), 2.0), Type));
        //Nubis3의 150미터 그대로 
        float HHFBlend = ValueRemap(Distance, 50.0, 150.0, 0.9, 1.0);
        NoiseComposite = lerp(HHFNoise, NoiseComposite, HHFBlend);
    }

    float Density = ValueErosion(Profile, NoiseComposite);

    float PoweredDensityScale = pow(saturate(DensityScale), 4.0);
    Density *= DensityScale; //PoweredDensityScale;

    // 경계 선명화
    Density = pow(Density, lerp(0.3, 0.6, max(kCloudEpsilon, PoweredDensityScale)));
    if (bHFDetails)
    {
        float HHFBlend = GetFractionFromValue(Distance, 50.0, 150.0);
        Density = pow(Density, lerp(0.5, 1.0, HHFBlend)) * lerp(0.666, 1.0, HHFBlend);
    }

    return Density;
}

// x = profile 밀도, y = 노이즈 계산 된 밀도
float2 GetVoxelCloudDensity(float3 Coord, float3 SamplePosition, float Distance)
{
    float3 Modeling = CloudModeling.SampleLevel(LinearClampSampler, ToVolumeUVW(Coord), 0).rgb;
    float Profile = Modeling.r;
    if (Profile <= 0.0)
        return 0.0;

    // 거리 기반 밉
    float MipLevel = log2(1.0 + Distance * MipDistanceScale);
    
    // 구름 가까워지면 투명하게 블렌딩
    float Full = GetUprezzedVoxelCloudDensity(SamplePosition, Distance, MipLevel, Profile, Modeling.g, Modeling.b, Distance < kHFDetailDistance)
        * ValueRemap(Distance, 10.0, 120.0, 0.25, 1.0);
    return float2(Profile * Modeling.b, Full);
}

float GetVoxelCloudShadowDensity(float3 Coord, float3 SamplePosition, float MipLevel)
{
    if (!IsInsideBounds(Coord))
        return 0.0;

    float3 Modeling = CloudModeling.SampleLevel(LinearClampSampler, ToVolumeUVW(Coord), 0).rgb;
    if (Modeling.r <= 0.0)
        return 0.0;

    return GetUprezzedVoxelCloudDensity(SamplePosition, kHFDetailDistance, MipLevel, Modeling.r, Modeling.g, Modeling.b, false);
}

// AABB
float2 RayBoxIntersect(float3 Origin, float3 Dir, float3 BoxMin, float3 BoxMax)
{
    float3 InvDir = 1.0 / Dir;
    float3 T0 = (BoxMin - Origin) * InvDir;
    float3 T1 = (BoxMax - Origin) * InvDir;
    float3 TMin = min(T0, T1);
    float3 TMax = max(T0, T1);
    return float2(max(max(TMin.x, TMin.y), max(TMin.z, 0.0)), min(min(TMax.x, TMax.y), TMax.z));
}

#endif