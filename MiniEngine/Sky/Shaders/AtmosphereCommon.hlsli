#ifndef ATMOSPHERE_COMMON_HLSLI
#define ATMOSPHERE_COMMON_HLSLI

static const float kPi = 3.14159265;
static const float kIsotropicPhase = 1.0 / (4.0 * kPi);
static const float kMultiScatteringLutSize = 32.0;
static const float2 kSkyViewLutSize = float2(192.0, 108.0); //1920*1080
static const float kAerialPerspectiveSliceCount = 32.0;
static const float kAerialPerspectiveMaxDistanceKm = 128.0;
static const float kInfiniteDistance = 1.0e9;

cbuffer AtmosphereConstants : register(b0)
{
    float3 RayleighScattering;
    float RayleighScaleHeight;//km
    
    float3 MieScattering;
    float MieScaleHeight;//km
    
    float3 MieExtinction;//scattering + absorption
    float MiePhaseG;
    
    float3 OzoneExtinction;
    float OzoneCenterHeight;
    
    float3 GroundAlbedo;
    float OzoneHalfWidth;
    
    //행성
    float BottomRadius;
    float TopRadius;
    float2 AtmosphereConstantsPad;
};

struct OpticalCoefficients
{
    float3 RayleighScattering;
    float3 MieScattering;
    float3 TotalExtinction;
};


// 고도 h에서 빛 감소 배율 및 산란도 계산
OpticalCoefficients GetOpticalCoefficients(float h)
{
    float RayleighDensity = exp(-h / RayleighScaleHeight);
    float MieDensity = exp(-h / MieScaleHeight);
    float OzoneDensity = max(0.0, 1.0 - abs(h - OzoneCenterHeight) / OzoneHalfWidth);
    
    OpticalCoefficients Coefficients;
    Coefficients.RayleighScattering = RayleighScattering * RayleighDensity;
    Coefficients.MieScattering = MieScattering * MieDensity;
    Coefficients.TotalExtinction = Coefficients.RayleighScattering
        + MieExtinction * MieDensity
        + OzoneExtinction * OzoneDensity;
    return Coefficients;
}

float GetSunVisibility(float3 Pos)
{
    return 1.0f;
}


// 레이와 구의 교차지점까지의 거리 중 먼 것
float RaySphereFar(float3 Origin, float3 Dir, float Radius)
{
    float DirOriginDot = dot(Dir, Origin);
    float SquaredDistance = Radius * Radius + DirOriginDot*DirOriginDot - dot(Origin, Origin);
    
    if (SquaredDistance < 0.0)
        return -1.0;
    return -DirOriginDot + sqrt(SquaredDistance);
}

//레이와 구의 교차지점까지의 거리 중 가까운 것
float RaySphereNear(float3 Origin, float3 Dir, float Radius)
{
    float DirOriginDot = dot(Dir, Origin);
    float SquaredDistance = Radius * Radius + DirOriginDot * DirOriginDot - dot(Origin, Origin);
    if (SquaredDistance < 0.0)
        return -1.0;
    float t = -DirOriginDot - sqrt(SquaredDistance);
    return (t >= 0.0) ? t : -1.0;
}

//대기 끝 마칭 구간이 적어 뒤틀리는 것 보정
bool MoveToTopAtmosphere(inout float3 Origin, float3 Dir)
{
    float Height = length(Origin);
    if (Height <= TopRadius)
        return true;

    float t = RaySphereNear(Origin, Dir, TopRadius);
    if (t < 0.0)
        return false;

    float3 HitPos = Origin + Dir * t;
    Origin = HitPos - normalize(HitPos) * 0.01;
    return true;
}

float RayleighPhase(float CosTheta)
{
    return 3.0 * (1 + CosTheta * CosTheta) / (16 * kPi);
}

float MiePhase(float CosTheta, float G)
{
    float G2 = G * G;
    float Upper = 3 * (1 - G2) * (1 + CosTheta * CosTheta);
    float Lower = 8 * kPi * (2 + G2) * pow(max(1.0 + G2 - 2.0 * G * CosTheta, 1e-4), 1.5);
    return Upper / Lower;
}

float HenyeyGreenstein(float CosTheta, float G)
{
    float G2 = G * G;
    return (1.0 - G2) / (4.0 * kPi * pow(max(1.0 + G2 - 2.0 * G * CosTheta, 1e-4), 1.5));
}

//비선형 맵핑이다. UV.y는 현재 위치의 지표면 기준 고도 [0,1] UV.x는 시선 방향으로 대기외부와의 거리[0,1]
//결과로 나오는 r은 현재 위치의 실제 고도, Mu는 천정각
void UvToRmu(float2 UV, out float r, out float Mu)
{
    //지평선 까지의 거리
    float H = sqrt(TopRadius * TopRadius - BottomRadius * BottomRadius);
    float Rho = H * UV.y;
    r = sqrt(Rho * Rho + BottomRadius * BottomRadius);

    float DMin = TopRadius - r;
    float DMax = Rho + H;
    float D = DMin + UV.x * (DMax - DMin);
    Mu = (D == 0.0) ? 1.0 : (H * H - Rho * Rho - D * D) / (2.0 * r * D);
    Mu = clamp(Mu, -1.0, 1.0);
}

float2 RmuToUv(float r, float Mu)
{
    float H = sqrt(TopRadius * TopRadius - BottomRadius * BottomRadius);
    float Rho = sqrt(max(0.0, r * r - BottomRadius * BottomRadius));

    float Discriminant = r * r * (Mu * Mu - 1.0) + TopRadius * TopRadius;
    float D = max(0.0, -r * Mu + sqrt(max(0.0, Discriminant)));

    float DMin = TopRadius - r;
    float DMax = Rho + H;
    return float2((D - DMin) / (DMax - DMin), Rho / H);
}

float3 SampleTransmittance(Texture2D<float4> Lut, SamplerState LinearClamp, float r, float Mu)
{
    return Lut.SampleLevel(LinearClamp, RmuToUv(r, Mu), 0).rgb;
}

float UnitToSubUv(float u, float Resolution)
{
    return (u + 0.5 / Resolution) * (Resolution / (Resolution + 1.0));
}

float SubUvToUnit(float u, float Resolution)
{
    return (u - 0.5 / Resolution) * (Resolution / (Resolution - 1.0));
}

// y는 고도 [0~1] 정규화, x는 태양각도 -1~1을 [0,1]로
float3 SampleMultiScattering(Texture2D<float4> Lut, SamplerState LinearClamp, float r, float SunMu)
{
    float2 UV = saturate(float2(SunMu * 0.5 + 0.5, (r - BottomRadius) / (TopRadius - BottomRadius)));
    UV = float2(UnitToSubUv(UV.x, kMultiScatteringLutSize), UnitToSubUv(UV.y, kMultiScatteringLutSize));
    return Lut.SampleLevel(LinearClamp, UV, 0).rgb;
}

float3 IntegrateScatteredLuminance(Texture2D<float4> TransmittanceLut, Texture2D<float4> MultiScatteringLut,
    SamplerState LinearClamp, float3 Origin, float3 Dir, float3 SunDir,
    float SunIlluminance, float MultiScatteringFactor, int StepCount,
    float MaxDistance, bool bUseSunVisibility, out float3 Throughput)
{
    Throughput = 1.0;

    float TopDistance = RaySphereFar(Origin, Dir, TopRadius);
    if (TopDistance <= 0.0)
        return 0.0;

    float GroundDistance = RaySphereNear(Origin, Dir, BottomRadius);
    float RayLength = (GroundDistance > 0.0) ? GroundDistance : TopDistance;
    RayLength = min(RayLength, MaxDistance);
    
    float CosTheta = dot(Dir, SunDir);
    float PhaseR = RayleighPhase(CosTheta);
    float PhaseM = MiePhase(CosTheta, MiePhaseG);

    float StepSize = RayLength / StepCount;
    float3 Luminance = 0.0;

    for (int i = 0; i < StepCount; ++i)
    {
        float3 SamplePos = Origin + Dir * ((i + 0.5) * StepSize);
        float r = length(SamplePos);
        OpticalCoefficients Coefficients = GetOpticalCoefficients(r - BottomRadius);

        float SunMu = dot(SamplePos / r, SunDir);//천정각
        float PlanetShadow = (RaySphereNear(SamplePos, SunDir, BottomRadius) > 0.0) ? 0.0 : 1.0;//행성과 닿은 경우 태양 가시성 0
        float3 SunTransmittance = SampleTransmittance(TransmittanceLut, LinearClamp, r, SunMu) * PlanetShadow;
        if (bUseSunVisibility)
            SunTransmittance *= GetSunVisibility(SamplePos);
        
        float3 MultiScattering = SampleMultiScattering(MultiScatteringLut, LinearClamp, r, SunMu) * MultiScatteringFactor;
        
        float3 InScattering = SunIlluminance * (
              SunTransmittance * (Coefficients.RayleighScattering * PhaseR + Coefficients.MieScattering * PhaseM)
            + MultiScattering * (Coefficients.RayleighScattering + Coefficients.MieScattering));//

        float3 StepTransmittance = exp(-Coefficients.TotalExtinction * StepSize);//구간 투과율
        Luminance += Throughput * (InScattering - InScattering * StepTransmittance) / max(Coefficients.TotalExtinction, 1e-6);
        Throughput *= StepTransmittance;
    }

    return Luminance;
}

//UV->Cos천정각,Cos태양죄우각
void UvToSkyViewParams(float2 UV, float ViewHeight, out float ViewZenithCos, out float LightViewCos)
{
    UV = float2(SubUvToUnit(UV.x, kSkyViewLutSize.x), SubUvToUnit(UV.y, kSkyViewLutSize.y));

    float HorizonDistance = sqrt(max(0.0, ViewHeight * ViewHeight - BottomRadius * BottomRadius));
    float Beta = acos(saturate(HorizonDistance / ViewHeight)); // 지평선에서 발 아래까지의 각도
    float ZenithHorizonAngle = kPi - Beta; // 천정각

    if (UV.y < 0.5)
    {
        float Coord = 1.0 - 2.0 * UV.y;
        Coord = 1.0 - Coord * Coord;
        ViewZenithCos = cos(ZenithHorizonAngle * Coord);
    }
    else
    {
        float Coord = UV.y * 2.0 - 1.0;
        ViewZenithCos = cos(ZenithHorizonAngle + Beta * Coord * Coord);
    }

    float CoordX = UV.x * UV.x;
    LightViewCos = -(CoordX * 2.0 - 1.0);
}

// 위 함수의 역 천정각+태양각도->UV
float2 SkyViewParamsToUv(bool bIntersectGround, float ViewZenithCos, float LightViewCos, float ViewHeight)
{
    float HorizonDistance = sqrt(max(0.0, ViewHeight * ViewHeight - BottomRadius * BottomRadius));
    float Beta = acos(saturate(HorizonDistance / ViewHeight));
    float ZenithHorizonAngle = kPi - Beta;
    float ViewZenithAngle = acos(clamp(ViewZenithCos, -1.0, 1.0));

    float2 UV;
    if (!bIntersectGround)
    {
        float Coord = 1.0 - saturate(ViewZenithAngle / ZenithHorizonAngle);
        UV.y = (1.0 - sqrt(Coord)) * 0.5;
    }
    else
    {
        float Coord = saturate((ViewZenithAngle - ZenithHorizonAngle) / Beta);
        UV.y = sqrt(Coord) * 0.5 + 0.5;
    }

    UV.x = sqrt(saturate(-LightViewCos * 0.5 + 0.5));

    return float2(UnitToSubUv(UV.x, kSkyViewLutSize.x), UnitToSubUv(UV.y, kSkyViewLutSize.y));
}

// 거리(km) -> Aerial Perspective 볼륨 
float4 SampleAerialPerspective(Texture3D<float4> Volume, SamplerState LinearClamp, float2 UV, float DistanceKm)
{
    float Slice = DistanceKm / kAerialPerspectiveMaxDistanceKm * kAerialPerspectiveSliceCount;
    float Weight = 1.0;
    if (Slice < 0.5)
    {
        Weight = saturate(Slice * 2.0);
        Slice = 0.5;
    }
    float W = sqrt(Slice / kAerialPerspectiveSliceCount);
    return Volume.SampleLevel(LinearClamp, float3(UV, W), 0) * Weight;
}

#endif