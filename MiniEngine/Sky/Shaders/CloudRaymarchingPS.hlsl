#include "AtmosphereCommon.hlsli"
#include "SkyViewCommon.hlsli"
#include "CloudCommon.hlsli"

Texture2D<float4> TransmittanceLut : register(t0);
Texture2D<float4> SkyViewLut : register(t1);
Texture3D<float4> AerialPerspectiveVolume : register(t2);
Texture2D<float> SceneDepth : register(t3);

Texture2D<float4>   PreviusCloud : register(t8);
RWTexture2D<float4> CurrentCloud : register(u0);
Texture2D<float>    PreviusCloudDepth : register(t9);
RWTexture2D<float>  CurrentCloudDepth : register(u1);

static const float kPhaseForwardG = 0.8;
static const float kPhaseBackwardG = -0.2;
static const float kLightStepStart = 16.0; //m
static const int kLightStepCount = 6;
static const int kLightErosionStepCount = 3;
static const float2 kLightJitterPixelOffset = float2(47.0, 17.0);
static const float kGoldenRatioFrac = 0.61803398875;

static const float kMinTransmittance = 0.01;
static const int kScatterOctaveCount = 3;
static const float kOctaveAttenuation = 0.5;
static const float kOctaveContribution = 0.5;
static const float kOctaveEccentricity = 0.5;

float GetDualLobePhase(float CosTheta, float Eccentricity)
{
    return lerp(HenyeyGreenstein(CosTheta, kPhaseForwardG * Eccentricity),
                HenyeyGreenstein(CosTheta, kPhaseBackwardG * Eccentricity), 0.5);
}

float GetSunScattering(float LightDepth, float CosTheta)
{
    float Result = 0.0;
    float a = 1.0, b = 1.0, c = 1.0;
    for (int o = 0; o < kScatterOctaveCount; ++o)
    {
        Result += b * exp(-LightDepth * a) * GetDualLobePhase(CosTheta, c);
        a *= kOctaveAttenuation;
        b *= kOctaveContribution;
        c *= kOctaveEccentricity;
    }
    return Result;
}

// 태양 방향 레이마칭
float GetLightOpticalDepth(float3 RelPos, float ViewDistance, float Jitter)
{
    float OpticalDepth = 0.0;
    float Distance = 0.0;
    float StepLength = kLightStepStart;
    for (int i = 0; i < kLightStepCount; ++i)
    {
        float3 SampleRelPos = RelPos + SunDirection * (Distance + StepLength * Jitter);
        float3 Coord = (SampleRelPos - BoundsMin) / BoundsSize;

        float Density = (i < kLightErosionStepCount)
            ? GetVoxelCloudShadowDensity(Coord, SampleRelPos + CameraPosition, GetNoiseMipFromFootprint(StepLength))
            : SampleCloudProfile(Coord);
        OpticalDepth += Density * StepLength;

        Distance += StepLength;
        StepLength *= 2.0;
    }
    return OpticalDepth * Extinction;
}

float3 GetSunTransmittance(float3 RelPos)
{
    float3 SamplePosKm = CameraPositionKm + RelPos * 0.001;
    float r = length(SamplePosKm);
    float SunMu = dot(SamplePosKm / r, SunDirection);
    float PlanetShadow = (RaySphereNear(SamplePosKm, SunDirection, BottomRadius) > 0.0) ? 0.0 : 1.0;
    return SampleTransmittance(TransmittanceLut, LinearClampSampler, r, SunMu) * PlanetShadow;
}

float4 main(float4 Position : SV_Position, float2 UV : TEXCOORD0) : SV_Target0
{
    uint2 Pixel = uint2(Position.xy);
    
    uint2 TargetSize;
    PreviusCloud.GetDimensions(TargetSize.x, TargetSize.y);
    float2 _UV = Position.xy / TargetSize;
    
    float3 Dir = GetWorldViewDir(_UV);
    float2 Hit = RayBoxIntersect(0.0, Dir, BoundsMin, BoundsMin + BoundsSize);

    //뎁스 버퍼로 길이 계산
    float SceneDistance = GetSceneViewDistance(_UV, SceneDepth[uint2(Position.xy)]) * WorldToMeters;
    Hit.y = min(Hit.y, SceneDistance);
    if (Hit.x >= Hit.y)
    {
        CurrentCloud[Pixel] = 0;
        CurrentCloudDepth[Pixel] = 0;
        return 0.0;
    }
    
    float ApproximatePreviusDepth = PreviusCloudDepth[Pixel]; //현재 프레임 기반 추정치
    
    float4 ClipPosition = (ApproximatePreviusDepth > 0.0)
    ? mul(PrevViewProj, float4(Dir * ApproximatePreviusDepth + CameraDeltaMeters, 1.0))
    : mul(PrevViewProj, float4(Dir , 0.0));
    float2 PreviusUV = float2(ClipPosition.x / ClipPosition.w * 0.5 + 0.5, 0.5 - ClipPosition.y / ClipPosition.w * 0.5);
    
    if (
        ((uint(Position.y) % 16 + uint(Position.x)) % 16 != FrameIndex % 16)
        &&ClipPosition.w>0.0 && all(PreviusUV>=0.0)&&all(PreviusUV<=1.0)//화면 밖 시 갱신    
        )
    {
        uint2 PreviusPixel = min(uint2(PreviusUV * TargetSize), TargetSize - 1);
        
        float4 PreviusColor = PreviusCloud[PreviusPixel];
        float PreviusDepth = PreviusCloudDepth[PreviusPixel];

        
        float CameraDeltaLength = length(CameraDeltaMeters);
        float CosThetaXCameraDeltaLength = dot(CameraDeltaMeters, Dir);

        //정확한 길이 계산
        float NewDepth = -CosThetaXCameraDeltaLength + sqrt(CosThetaXCameraDeltaLength * CosThetaXCameraDeltaLength - CameraDeltaLength * CameraDeltaLength + PreviusDepth * PreviusDepth);
        //근사
        //float NewDepth = PreviusDepth - dot(Dir, CameraDeltaMeters);
        
        CurrentCloud[Pixel] = PreviusColor;
        CurrentCloudDepth[Pixel] = PreviusDepth > 0.0 ? max(NewDepth, 0.001) : 0.0;
        return PreviusColor;
    }


    // 천정 방향 하늘 휘도를 환경광으로 
    float ViewHeight = length(CameraPositionKm);
    float3 SkyAmbient = SkyViewLut.SampleLevel(LinearClampSampler, SkyViewParamsToUv(false, 1.0, 1.0, ViewHeight), 0).rgb * AmbientScale;

    float CosTheta = dot(Dir, SunDirection);
    float Phase = lerp(HenyeyGreenstein(CosTheta, kPhaseForwardG), HenyeyGreenstein(CosTheta, kPhaseBackwardG), 0.5);

    float Transmittance = 1.0;
    float3 Luminance = 0.0;
    float WeightedDistance = 0.0;
    float WeightSum = 0.0;
    
    float LightJitter = InterleavedGradientNoise(Position.xy + kLightJitterPixelOffset, FrameIndex);
    
    float t = Hit.x +InterleavedGradientNoise(Position.xy, FrameIndex) * max(1.0, sqrt(Hit.x) * StepScale);
    for (uint i = 0; i < MaxSteps && t < Hit.y; ++i)
    {
        float3 RelPos = Dir * t;
        float3 Coord = (RelPos - BoundsMin) / BoundsSize;
        float BaseStep = max(1.0, sqrt(t) * StepScale);

        // SDF로 빈 공간 건너뛰기
        float Sdf = SampleCloudSdf(Coord);
        if (Sdf > BaseStep)
        {
            t += Sdf;
            continue;
        }

        float Density = GetVoxelCloudDensity(Coord, RelPos + CameraPosition, t).y;
        if (Density > 0.0)
        {
            float StepTransmittance = exp(-Density * Extinction * BaseStep);

            // Multi Scattering 근사
            float LightDepth = GetLightOpticalDepth(RelPos, t, frac(LightJitter + i * kGoldenRatioFrac));
            
            float3 StepLuminance = SunIlluminance * GetSunTransmittance(RelPos) * GetSunScattering(LightDepth, CosTheta)
                + SkyAmbient * lerp(0.5, 1.0, Coord.y);
            
            float Weight = Transmittance * (1.0 - StepTransmittance);
            Luminance += StepLuminance * Weight;
            WeightedDistance += t * Weight;
            WeightSum += Weight;

            Transmittance *= StepTransmittance;
            if (Transmittance < kMinTransmittance)
                break;
        }
        t += BaseStep;
    }

    float Alpha = 1.0 - Transmittance;
    if (WeightSum > 0.0)
    {
        // Aerial Perspective 
        float4 AerialPerspective = SampleAerialPerspective(AerialPerspectiveVolume, LinearClampSampler, UV, WeightedDistance / WeightSum * 0.001);

        //구름 끼리의 그림자 도입 시 우선 적용 해봐야함
        //AerialPerspective.rgb -= SampleAtmosphereShadow(AerialPerspectiveVolume, LinearClampSampler, UV, WeightedDistance / WeightSum * 0.001);
        
        Luminance = Luminance * (1.0 - AerialPerspective.a) + AerialPerspective.rgb * Alpha;
    }
    float4 ret = float4(Luminance, Alpha);
    CurrentCloud[Pixel] = ret;
    CurrentCloudDepth[(uint2) Pixel] = WeightSum > 0 ? WeightedDistance / WeightSum : 0;
    return ret;
}