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

//Amotization PS에서 이번 프레임에 갱신된 1/N해상도 텍스쳐 
Texture2D<float4> CloudRenderTarget : register(t10);
Texture2D<float> CloudDepthRenderTarget : register(t11);






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
        ((uint(Position.y) % kAmortizationSize * kAmortizationSize + uint(Position.x)) % kAmortizationSize * kAmortizationSize != FrameIndex % kAmortizationSize * kAmortizationSize)
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

    float4 ret = float4(Luminance, Alpha);
    CurrentCloud[Pixel] = ret;
    CurrentCloudDepth[(uint2) Pixel] = WeightSum > 0 ? WeightedDistance / WeightSum : 0;
    return ret;
}