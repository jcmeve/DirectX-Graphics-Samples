struct VSOutput
{
    float4 Position : SV_Position;
    float2 UV : TEXCOORD0;
};

VSOutput main(uint VertID : SV_VertexID)
{
    VSOutput Output;
    Output.UV = float2(uint2(VertID, VertID << 1) & 2);
    Output.Position = float4(lerp(float2(-1, 1), float2(1, -1), Output.UV), 0, 1);
    return Output;
}