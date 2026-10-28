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
SamplerState LinearClampSampler : register(s0);
SamplerState LinearWrapSampler : register(s1);

static const float kSdfMinMeters = -256.0;
static const float kSdfMaxMeters = 4096.0;
static const float kNoiseFrequency = 0.01; // 노이즈 100m 타일
static const float kHFDetailDistance = 150.0;
static const float kCloudEpsilon = 1e-4;

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

float GetUprezzedVoxelCloudDensity(float3 SamplePosition, float Distance, float Profile, float Type, float DensityScale, bool bHFDetails)
{
    // 거리 기반 밉
    float MipLevel = log2(1.0 + Distance * MipDistanceScale);
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
    Density *= PoweredDensityScale;

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

    // 구름 가까워지면 투명하게 블렌딩
    float Full = GetUprezzedVoxelCloudDensity(SamplePosition, Distance, Profile, Modeling.g, Modeling.b, Distance < kHFDetailDistance)
        * ValueRemap(Distance, 10.0, 120.0, 0.25, 1.0);
    return float2(Profile * Modeling.b, Full);
}

#endif