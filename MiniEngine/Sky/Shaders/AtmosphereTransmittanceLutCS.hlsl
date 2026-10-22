#include "AtmosphereCommon.hlsli"

static const int kTransmittanceSteps = 40;

RWTexture2D<float4> TransmittanceLut : register(u0);

[numthreads(8, 8, 1)]
void main( uint3 DTid : SV_DispatchThreadID )
{
    uint2 LutSize;
    TransmittanceLut.GetDimensions(LutSize.x, LutSize.y);
    
    if (DTid.x >= LutSize.x || DTid.y >= LutSize.y)
        return;
    
    float2 UV = (DTid.xy + 0.5) / LutSize;
    float r, Mu;
    UvToRmu(UV, r, Mu);

    float3 Origin = float3(0.0, r, 0.0);
    float3 Dir = float3(sqrt(saturate(1.0 - Mu * Mu)), Mu, 0.0);
    float RayLength = RaySphereFar(Origin, Dir, TopRadius);

    float StepSize = RayLength / kTransmittanceSteps;
    float3 OpticalDepth = 0.0;
    for (int i = 0; i < kTransmittanceSteps; ++i)
    {
        float3 SamplePos = Origin + Dir * ((i + 0.5) * StepSize);
        float Height = length(SamplePos) - BottomRadius;
        //³ôÀÌÀÇ ºû °¨¼ÒÀ² * µ¨Å¸
        OpticalDepth += GetExtinction(Height) * StepSize;
    }

    TransmittanceLut[DTid.xy] = float4(exp(-OpticalDepth), 1.0);
    

}