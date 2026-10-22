#ifndef ATMOSPHERE_COMMON_HLSLI
#define ATMOSPHERE_COMMON_HLSLI

static const float kPi = 3.14159265;

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

//비선형 맵핑이다. UV.y는 현재 위치가 수평선 방향으로 떨어진 거리 [0,1] UV.x는 시선 방향으로 대기외부와의 거리[0,1]
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

//r,Mu에서의 총 투과율
float3 SampleTransmittance(Texture2D<float4> Lut, SamplerState LinearClamp, float r, float Mu)
{
    return Lut.SampleLevel(LinearClamp, RmuToUv(r, Mu), 0).rgb;
}

#endif