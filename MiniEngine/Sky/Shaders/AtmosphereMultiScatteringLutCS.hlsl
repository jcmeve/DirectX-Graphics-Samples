#include "AtmosphereCommon.hlsli"

static const uint kSqrtDirectionCount = 8;
static const uint kDirectionCount = kSqrtDirectionCount * kSqrtDirectionCount;
static const int kMultiScatteringSteps = 20;

Texture2D<float4> TransmittanceLut : register(t0);
SamplerState LinearClampSampler : register(s0);
RWTexture2D<float4> MultiScatteringLut : register(u0);

groupshared float3 s_SecondOrder[kDirectionCount];
groupshared float3 s_Transfer[kDirectionCount];

void IntegrateDirection(float3 Origin, float3 Dir, float3 SunDir, out float3 SecondOrder, out float3 Transfer)
{
    SecondOrder = 0.0;
    Transfer = 0.0;

    float TopDistance = RaySphereFar(Origin, Dir, TopRadius);
    float GroundDistance = RaySphereNear(Origin, Dir, BottomRadius);
    bool bHitsGround = GroundDistance > 0.0;
    float RayLength = bHitsGround ? GroundDistance : TopDistance;
    if (RayLength <= 0.0)
        return;

    float StepSize = RayLength / kMultiScatteringSteps;
    float3 Throughput = 1.0;

    for (int i = 0; i < kMultiScatteringSteps; ++i)
    {
        float3 SamplePos = Origin + Dir * ((i + 0.5) * StepSize);
        float r = length(SamplePos);
        OpticalCoefficients Coefficients = GetOpticalCoefficients(r - BottomRadius);

        float3 Scattering = Coefficients.RayleighScattering + Coefficients.MieScattering;
        float3 Extinction = max(Coefficients.TotalExtinction, 1e-6);
        float3 StepTransmittance = exp(-Coefficients.TotalExtinction * StepSize);

        float SunMu = dot(SamplePos / r, SunDir);
        float PlanetShadow = (RaySphereNear(SamplePos, SunDir, BottomRadius) > 0.0) ? 0.0 : 1.0;
        float3 SunTransmittance = SampleTransmittance(TransmittanceLut, LinearClampSampler, r, SunMu) * PlanetShadow;

        // 2차 산란의 입력: 이 지점에서 등방으로 한 번 산란된 태양빛
        float3 InScattering = SunTransmittance * Scattering * kIsotropicPhase;
        SecondOrder += Throughput * (InScattering - InScattering * StepTransmittance) / Extinction;

        // 전달 계수: 구 전체에서 적분값이 1인 빛이 들어왔을 때 다시 산란되는 양
        Transfer += Throughput * (Scattering - Scattering * StepTransmittance) / Extinction;

        Throughput *= StepTransmittance;
    }

    // 지면 반사: 땅에 닿은 태양빛이 람베르트 반사로 다시 대기로 올라감
    if (bHitsGround)
    {
        float3 GroundPos = Origin + Dir * GroundDistance;
        float GroundR = length(GroundPos);
        float GroundSunMu = dot(GroundPos / GroundR, SunDir);
        float3 GroundSunTransmittance = SampleTransmittance(TransmittanceLut, LinearClampSampler, GroundR, GroundSunMu);
        SecondOrder += Throughput * GroundSunTransmittance * saturate(GroundSunMu) * GroundAlbedo / kPi;
    }
}

[numthreads(1, 1, kDirectionCount)]
void main( uint3 DTid : SV_DispatchThreadID )
{
    uint2 LutSize;
    MultiScatteringLut.GetDimensions(LutSize.x, LutSize.y);
    
    float2 UV = (DTid.xy + 0.5) / LutSize;
    UV = float2(SubUvToUnit(UV.x, LutSize.x), SubUvToUnit(UV.y, LutSize.y));
    
    float Mu = UV.x * 2.0f - 1.0f;//0~1 -> -1~1
    float3 SunDir = float3(sqrt(saturate(1.0 - Mu * Mu)), Mu, 0.0);
    float r = BottomRadius + saturate(UV.y + 0.01) * (TopRadius - BottomRadius-0.01);
    float3 Origin = float3(0.0, r, 0.0);
    
    //360도 두 축으로 8개씩 총 64곳 검사
    uint DirIndex = DTid.z;
    float RandA = (DirIndex / kSqrtDirectionCount + 0.5) / kSqrtDirectionCount;
    float RandB = (DirIndex % kSqrtDirectionCount + 0.5) / kSqrtDirectionCount;
    float Theta = 2.0 * kPi * RandA;
    float Phi = acos(1.0 - 2.0 * RandB);
    float3 Dir = float3(cos(Theta) * sin(Phi), cos(Phi), sin(Theta) * sin(Phi));
    
    float3 SecondOrder, Transfer;
    IntegrateDirection(Origin, Dir, SunDir, SecondOrder, Transfer);
    
    //입체각으로 각 조사지점 적분
    const float SolidAnglePerDirection = 4.0 * kPi / kDirectionCount; //입체각
    s_SecondOrder[DirIndex] = SecondOrder * SolidAnglePerDirection;
    s_Transfer[DirIndex] = Transfer * SolidAnglePerDirection;
    
    GroupMemoryBarrierWithGroupSync();
    if(DirIndex!=0)
        return;
    float3 SecondOrderSum = 0.0;
    float3 TransferSum = 0.0;
    for (uint k = 0; k < kDirectionCount; ++k)
    {
        SecondOrderSum += s_SecondOrder[k];
        TransferSum += s_Transfer[k];
    }
    
    float3 L2 = SecondOrderSum * kIsotropicPhase;
    float3 Fms = TransferSum * kIsotropicPhase;
    
    float3 MultiScattering = L2 / (1.0 - Fms);
    MultiScatteringLut[DTid.xy] = float4(MultiScattering, 1.0);
}