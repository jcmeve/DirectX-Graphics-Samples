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


float4 ClipToAABB(float4 History, float4 BoxMin, float4 BoxMax)
{
    float4 Center = (BoxMin + BoxMax) * 0.5;
    float4 Extent = (BoxMax - BoxMin) * 0.5 + 1e-5;
    float4 Offset = History - Center;
    float4 Unit = abs(Offset / Extent);
    float MaxUnit = max(max(Unit.x, Unit.y), max(Unit.z, Unit.w));
    return (MaxUnit > 1.0) ? Center + Offset / MaxUnit : History;

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
    
    float ApproximatePreviusDepth = PreviusCloudDepth[Pixel];//[Pixel]; //현재 프레임 기반 추정치
    
    float4 ClipPosition = (ApproximatePreviusDepth > 0.0)
    ? mul(PrevViewProj, float4(Dir * ApproximatePreviusDepth + CameraDeltaMeters, 1.0))
    : mul(PrevViewProj, float4(Dir , 0.0));
    float2 PreviusUV = float2(ClipPosition.x / ClipPosition.w * 0.5 + 0.5, 0.5 - ClipPosition.y / ClipPosition.w * 0.5);

    uint2 LowSize;
    CloudRenderTarget.GetDimensions(LowSize.x, LowSize.y);
    uint2 LowPixel = min(Pixel / kAmortizationSize, LowSize - 1);
    
    //이번 프레임에 갱신된 경우 사용
    if(all((Pixel % kAmortizationSize) == GetAmortizationOffset(FrameIndex)))
    {
        float4 Color = CloudRenderTarget[LowPixel];
        CurrentCloud[Pixel] = Color;
        CurrentCloudDepth[Pixel] = CloudDepthRenderTarget[LowPixel];
        return Color;
    }
    
    //갱신 안됐으나 이전 프레임에 화면 안에 있었던 경우 Reprojection
    if (ClipPosition.w > 0.0 && all(PreviusUV >= 0.0) && all(PreviusUV <= 1.0))
    {
        uint2 PreviusPixel = min(uint2(PreviusUV * TargetSize), TargetSize - 1);
        
        float4 PreviusColor = PreviusCloud.SampleLevel(LinearClampSampler, PreviusUV, 0);
        float CloudDepth = CloudDepthRenderTarget[LowPixel];
        
        float4 MinColor = 987654321.0;
        float4 MaxColor = -987654321.0;
        for (int y = -1; y <= 1; ++y)
        {
            for (int x = -1; x <= 1; ++x)
            {
                int2 SamplePixel = clamp(int2(LowPixel) + int2(x, y), 0, int2(LowSize) - 1);
                float4 SampleColor = CloudRenderTarget[SamplePixel];
                
                CloudDepth = max(CloudDepth, CloudDepthRenderTarget[SamplePixel]);
                
                MinColor = min(MinColor, SampleColor);
                MaxColor = max(MaxColor, SampleColor);
            }
        }
        float4 CloudColor = clamp(PreviusColor, MinColor, MaxColor);
        
        float MotionPixels = length(PreviusPixel - Pixel);
        float ClampWeight = saturate(MotionPixels / 1);
        CloudColor = lerp(PreviusColor, CloudColor, ClampWeight);

        CurrentCloud[Pixel] = CloudColor;
        CurrentCloudDepth[Pixel] = CloudDepth;
        return CloudColor;
    }
    
    //화면 밖에 있었고 갱신도 되지 않은 경우 샘플링에 의존
    float4 Color = CloudRenderTarget.SampleLevel(LinearClampSampler, _UV, 0);
    CurrentCloud[Pixel] = Color;
    CurrentCloudDepth[Pixel] = CloudDepthRenderTarget[LowPixel];
    return Color;
}