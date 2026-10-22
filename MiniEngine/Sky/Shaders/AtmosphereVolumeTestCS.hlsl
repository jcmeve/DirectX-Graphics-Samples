RWTexture3D<float4> VolumeOutput : register(u0);

[numthreads(4, 4, 4)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    uint3 VolumeSize;
    VolumeOutput.GetDimensions(VolumeSize.x, VolumeSize.y, VolumeSize.z);
    if (any(DTid >= VolumeSize))
        return;

    VolumeOutput[DTid] = float4((DTid + 0.5) / VolumeSize, 1.0);
}