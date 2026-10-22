#ifndef ATMOSPHERE_COMMON_HLSLI
#define ATMOSPHERE_COMMON_HLSLI

cbuffer AtmosphereConstants : register(b0)
{
    float3 RayleighScattering;
    float RayleighScaleHeight;//km
    
    float3 MieScattering;
    float MieScaleHeight;//km
    
    float3 MieAbsorption;
    float MiePhaseG;
    
    float3 OzoneAbsorption;
    float OzoneCenterHeight;
    
    float3 GroundAlbedo;
    float OzoneHalfWidth;
    
    //행성
    float BottomRadius;
    float TopRadius;
    float2 Pad;
};

// 고도 h에서 빛 감소 배율
float3 GetExtinction(float h)
{
    float RayleighDensity = exp(-h / RayleighScaleHeight);
    float MieDensity = exp(-h / MieScaleHeight);
    float OzoneDensity = max(0.0, 1.0 - abs(h - OzoneCenterHeight) / OzoneHalfWidth);

    return RayleighScattering * RayleighDensity
        + (MieScattering + MieAbsorption) * MieDensity
        + OzoneAbsorption * OzoneDensity;
}

// 레이와 구의 교차지점까지의 거리 
float RaySphereFar(float3 Origin, float3 Dir, float Radius)
{
  
    float dirOriginDot = dot(Dir, Origin);
    float distance = sqrt(Radius * Radius + dirOriginDot*dirOriginDot - dot(Origin, Origin));
    
    if (distance < 0.0)
        return -1.0;
    return -dirOriginDot + sqrt(distance);
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


#endif