RWTexture3D<uint> Target : register(u0);

[numthreads(4, 4, 4)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    Target[DTid] = 0;
}