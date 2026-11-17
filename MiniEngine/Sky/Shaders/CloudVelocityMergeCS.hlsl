#include "../../Core/Shaders/PixelPacking_Velocity.hlsli"

Texture2D<uint> CloudVelocity : register(t0);
RWTexture2D<uint> VelocityBuffer : register(u0);

static const uint kNoCloudVelocity = 0xFFFFFFFF;


[numthreads(8, 8, 1)]
void main( uint3 DTid : SV_DispatchThreadID )
{
    uint2 Size;
    CloudVelocity.GetDimensions(Size.x, Size.y);
    if(any(DTid.xy>=Size))
        return;
    uint Packed = CloudVelocity[DTid.xy];
    if (Packed != kNoCloudVelocity)
        VelocityBuffer[DTid.xy] = Packed;
}