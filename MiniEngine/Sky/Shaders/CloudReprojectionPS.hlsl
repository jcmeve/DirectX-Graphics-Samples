#include "AtmosphereCommon.hlsli"
#include "SkyViewCommon.hlsli"
#include "CloudCommon.hlsli"
#include "../../Core/Shaders/PixelPacking_Velocity.hlsli"

Texture2D<float4> TransmittanceLut : register(t0);
Texture2D<float4> SkyViewLut : register(t1);
Texture3D<float4> AerialPerspectiveVolume : register(t2);
Texture2D<float> SceneDepth : register(t3);

Texture2D<float4>   PreviusCloud : register(t8);
RWTexture2D<float4> CurrentCloud : register(u0);

RWTexture2D<uint> CloudVelocity : register(u2);

//Amotization PS에서 이번 프레임에 갱신된 1/N해상도 텍스쳐 
Texture2D<float4> CloudRenderTarget : register(t10);
Texture2D<float> CloudDepthRenderTarget : register(t11);

static const float kColorChangeSensitive = 50.0;//20~30
static const float kMotionSensitive = 0.1;
static const float kMarchedColorChangeSensitive = 20.0;

static const uint kNoCloudVelocity = 0xFFFFFFFF;
static const float kVelocityAlphaThreshold = 1.0 / 255.0;

float2 GetPreviusUV(float3 Dir, float Depth, out float4 ClipPosition)
{
    ClipPosition = (Depth > 0.0)
        ? mul(PrevViewProj, float4(Dir * Depth + CameraDeltaMeters - GlobalWindDeltaMeters, 1.0))
        : mul(PrevViewProj, float4(Dir, 0.0));
    return float2(ClipPosition.x / ClipPosition.w * 0.5 + 0.5, 0.5 - ClipPosition.y / ClipPosition.w * 0.5);
}

float GetPreviusAlpha(float2 PreviusUV)
{
    return (all(PreviusUV >= 0.0) && all(PreviusUV <= 1.0)) ? PreviusCloud.SampleLevel(LinearClampSampler, PreviusUV, 0).a : 0.0;
}

void WriteCloudVelocity(uint2 Pixel, float Alpha, float PreviusAlpha, float2 PreviusPixelPos, float2 CurrentPixelPos)
{
    CloudVelocity[Pixel] = (max(Alpha, PreviusAlpha) > kVelocityAlphaThreshold)
        ? PackVelocity(float3(PreviusPixelPos - CurrentPixelPos, 0.0))
        : kNoCloudVelocity;
}

float GetNeighborMaxDepth(int2 LowPixel, int2 LowSize)
{
    float MaxDepth = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            MaxDepth = max(MaxDepth, CloudDepthRenderTarget[clamp(LowPixel + int2(x, y), 0, LowSize - 1)]);
    return MaxDepth;
}

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
        CurrentCloud[Pixel] = 0.0;
        CloudVelocity[Pixel] = kNoCloudVelocity;
        return 0.0;
    }
    
    uint2 LowSize;
    CloudRenderTarget.GetDimensions(LowSize.x, LowSize.y);
    uint2 LowPixel = min(Pixel / kAmortizationSize, LowSize - 1);
    uint2 SampleOffset = GetAmortizationOffset(FrameIndex);
    float CloudDepth = 0;
    float2 PreviusUV = 0;
    float4 ClipPosition = 0;
    float VelocityDepth = GetNeighborMaxDepth(LowPixel, LowSize);
    //이번 프레임에 갱신된 경우 사용
    if (all((Pixel % kAmortizationSize) == SampleOffset))
    {
        CloudDepth = CloudDepthRenderTarget[LowPixel];
        float4 Color = CloudRenderTarget[LowPixel];
        PreviusUV = GetPreviusUV(Dir, VelocityDepth, ClipPosition);

        CurrentCloud[Pixel] = Color;
        WriteCloudVelocity(Pixel, Color.a, GetPreviusAlpha(PreviusUV), PreviusUV * TargetSize, Position.xy);
        return Color;
    }
    float2 LowUV = ((Position.xy - SampleOffset - 0.5) / kAmortizationSize + 0.5) / LowSize;
    float4 MarchedNearColor = CloudRenderTarget.SampleLevel(LinearClampSampler, LowUV, 0);
    
    
    //Clamping
    //float4 MinColor = 987654321.0;
    //float4 MaxColor = -987654321.0;
    
    //for (int y = -1; y <= 1; ++y)
    //{
    //    for (int x = -1; x <= 1; ++x)
    //    {
    //        int2 SamplePixel = clamp(int2(LowPixel) + int2(x, y), 0, int2(LowSize) - 1);
    //        float4 SampleColor = CloudRenderTarget[SamplePixel];
                
    //        CloudDepth = max(CloudDepth, CloudDepthRenderTarget[SamplePixel]);
                
    //        MinColor = min(MinColor, SampleColor);
    //        MaxColor = max(MaxColor, SampleColor);
    //    }
    //}
    
    //Clipping
    float4 Moment1 = 0.0;
    float4 Moment2 = 0.0;
        
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            int2 SamplePixel = clamp(int2(LowPixel) + int2(x, y), 0, int2(LowSize) - 1);
            float4 SampleColor = CloudRenderTarget[SamplePixel];
            Moment1 += SampleColor;
            Moment2 += SampleColor * SampleColor;
        }
    }
    CloudDepth = VelocityDepth;
    Moment1 /= 9.0;
    Moment2 /= 9.0;
    float4 Sigma = sqrt(max(Moment2 - Moment1 * Moment1, 0.0));
    static const float kVarianceScale = 1.1;
    float4 BoxMin = Moment1 - Sigma * kVarianceScale;
    float4 BoxMax = Moment1 + Sigma * kVarianceScale;
    
    PreviusUV = GetPreviusUV(Dir, CloudDepth, ClipPosition);

    //이전 프레임에 화면 밖이었던 경우 가까운 갱신값 사용
    if (!(ClipPosition.w > 0.0 && all(PreviusUV >= 0.0) && all(PreviusUV <= 1.0)))
    {
        CurrentCloud[Pixel] = MarchedNearColor;
        WriteCloudVelocity(Pixel, MarchedNearColor.a, 0.0, PreviusUV * TargetSize, Position.xy);
        return MarchedNearColor;
    }
    
    float4 PreviusColor = PreviusCloud.SampleLevel(LinearClampSampler, PreviusUV, 0); 
    float MotionPixels = length(PreviusUV * TargetSize - float2(Pixel));
    float ClampWeight = saturate(MotionPixels * kMotionSensitive);
    float4 ReprojectionColor = lerp(PreviusColor, ClipToAABB(PreviusColor, BoxMin, BoxMax), ClampWeight);
    
    float ColorChanged = length(PreviusColor - ReprojectionColor) / (length(PreviusColor) + length(ReprojectionColor) + 1e-5);
    //float HistoryWeight = saturate(1.0 - max(ColorChanged - 0.01, 0.0) * kColorChangeSensitive);
    
    float2 LowTexel = LowUV * LowSize + 0.5;
    float2 LowFrac = frac(LowTexel);
    float MarchedColorChanged = 0.0;
    for (int BlockY = 0; BlockY <= 1; ++BlockY)
    {
        for (int Blockx = 0; Blockx <= 1; ++Blockx)
        {
            int2 MarchedLowPixel = clamp(int2(floor(LowTexel)) + int2(Blockx, BlockY), 0, int2(LowSize) - 1);
            float2 MarchedPixelCenter = float2(MarchedLowPixel * kAmortizationSize + SampleOffset) + 0.5;
            float2 MarchedPreviusUV = PreviusUV + (MarchedPixelCenter - Position.xy) / TargetSize;
            float4 MarchedCurrent = CloudRenderTarget[MarchedLowPixel];
            float4 MarchedPrevius = PreviusCloud.SampleLevel(LinearClampSampler, MarchedPreviusUV, 0);
            
            float BilinearWeight = (Blockx ? LowFrac.x : 1.0 - LowFrac.x) * (BlockY ? LowFrac.y : 1.0 - LowFrac.y);
            MarchedColorChanged += BilinearWeight * length(MarchedCurrent - MarchedPrevius) / (length(MarchedCurrent) + length(MarchedPrevius) + 1e-5);

        }
    }
    MarchedColorChanged *= 1.0 - ClampWeight;
    float HistoryWeight = saturate(1.0 - max(max(ColorChanged - 0.01, 0.0) * kColorChangeSensitive, MarchedColorChanged*kMarchedColorChangeSensitive));
    
    float4 FinalColor = lerp(MarchedNearColor, ReprojectionColor, HistoryWeight);
    //if (FinalColor.a > 0.1)
    //    //FinalColor = float4(saturate(pow(abs((MarchedNearColor - ReprojectionColor).r) + 1, 1.0) - 1), saturate(pow(abs((MarchedNearColor - ReprojectionColor).g) + 1, 1.0) - 1), saturate(pow(abs((MarchedNearColor - ReprojectionColor).b) + 1, 1.0) - 1), 1.0);
    //    FinalColor = float4(HistoryWeight, 0.0, 0.0, 1);
    //else
    //    FinalColor = float4(0.2, 0.2, 0.2, 1.0);
    CurrentCloud[Pixel] = FinalColor;
    WriteCloudVelocity(Pixel, FinalColor.a, PreviusColor.a, PreviusUV * TargetSize, Position.xy);
    return FinalColor;
}