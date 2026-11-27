#include "CloudCommon.hlsli"

cbuffer OccupancyConstants : register(b5)
{
    int3 RegionMin;
    uint bFullRebuild;
    
    int3 WindowCellMin;
    uint OccupancyPad;
}

Texture3D<uint> ChangeMask : register(t9);
RWTexture3D<uint> VoxelBits : register(u0);
RWTexture3D<uint> BrickCount : register(u1);
RWTexture3D<uint> SectorCount : register(u2);

// 32바이트 비트마스크 범위 x + 4y + 16z
static const int3 kTexelVoxels = int3(4, 4, 2);
static const uint kTexelBits = 32;
static const int kDisplacementCellVoxels = 2;

int3 GetBitVoxel(uint Bit)
{
    return int3(Bit % 4, (Bit / 4) % 4, Bit / 16);
}

bool IsDisplacementChanged(int3 Texel)
{
    uint3 WindowCells;
    ChangeMask.GetDimensions(WindowCells.x, WindowCells.y, WindowCells.z);

    int3 CellMin = Texel * kTexelVoxels / kDisplacementCellVoxels - 1;
    int3 CellMax = (Texel + 1) * kTexelVoxels / kDisplacementCellVoxels;

    for (int z = CellMin.z; z <= CellMax.z; ++z)
    {
        for (int y = CellMin.y; y <= CellMax.y; ++y)
        {
            for (int x = CellMin.x; x <= CellMax.x; ++x)
            {
                int3 Local = int3(x, y, z) - WindowCellMin;
                if (any(Local < 0) || any(Local >= int3(WindowCells)))
                    continue;
                if (ChangeMask[Local] != 0)
                    return true;
            }
        }
    }
    
    return false;
}


uint ComputeVoxelBits(int3 Texel)
{
    uint3 VoxelCount;
    CloudModeling.GetDimensions(VoxelCount.x, VoxelCount.y, VoxelCount.z);

    int3 TexelVoxel = Texel * kTexelVoxels;
    uint Bits = 0;
    [unroll]
    for (uint Bit = 0; Bit < kTexelBits; ++Bit)
    {
        float3 UVW = (float3(TexelVoxel + GetBitVoxel(Bit)) + 0.5) / float3(VoxelCount);
        float3 Modeling = SampleDisplacedModeling(UVW).Modeling;
        if (Modeling.r * Modeling.b > 0.0)
            Bits |= 1u << Bit;
    }
    return Bits;
}

void AddBrickCount(int3 Brick, int Delta)
{
    uint3 BrickTotal;
    BrickCount.GetDimensions(BrickTotal.x, BrickTotal.y, BrickTotal.z);

    if (Brick.z < 0 || Brick.z >= int(BrickTotal.z))
        return;
    Brick.xy = (Brick.xy + int2(BrickTotal.xy)) % int2(BrickTotal.xy);

    uint Prev;
    InterlockedAdd(BrickCount[Brick], uint(Delta), Prev);
    int After = int(Prev) + Delta;

    //개수가 변한 경우 섹터도 변경
    uint3 Sector = uint3(Brick) / kSectorBricks;
    if (Prev == 0 && After != 0)
        InterlockedAdd(SectorCount[Sector], 1u);
    else if (Prev != 0 && After == 0)
        InterlockedAdd(SectorCount[Sector], 0xFFFFFFFFu); //-1
}

struct AxisNeighbor
{
    int Offset; //이웃 brick 방향 (-1, +1, 0=없음)
    uint FaceBits; //이웃 brick halo에 들어가는 비트
};

AxisNeighbor GetAxisNeighbor(uint Axis, int VoxelInBrick)
{
    AxisNeighbor Ret;
    Ret.Offset = 0;
    Ret.FaceBits = 0;

    int FaceVoxel = -1;
    if (VoxelInBrick == 0)
    {
        Ret.Offset = -1;
        FaceVoxel = 0;
    }
    else if (VoxelInBrick + kTexelVoxels[Axis] == int(kBrickVoxels))
    {
        Ret.Offset = 1;
        FaceVoxel = kTexelVoxels[Axis] - 1;
    }

    for (uint Bit = 0; Bit < kTexelBits; ++Bit)
    {
        if (GetBitVoxel(Bit)[Axis] == FaceVoxel)
            Ret.FaceBits |= 1u << Bit;
    }
    return Ret;
}

void ApplyBitChange(int3 Texel, uint OldBits, uint NewBits)
{
    uint Added = NewBits & ~OldBits;
    uint Removed = OldBits & ~NewBits;

    int3 TexelVoxel = Texel * kTexelVoxels;
    int3 Brick = TexelVoxel / int(kBrickVoxels);
    int3 InBrick = TexelVoxel % int(kBrickVoxels);

    AxisNeighbor Neighbor[3] =
    {
        GetAxisNeighbor(0, InBrick.x),
        GetAxisNeighbor(1, InBrick.y),
        GetAxisNeighbor(2, InBrick.z)
    };

    for (uint i = 0; i < 8; ++i)
    {
        uint3 Use = uint3(i & 1, (i >> 1) & 1, i >> 2);

        uint Bits = ~0u;
        int3 Offset = 0;
        bool bValid = true;
        for (uint Axis = 0; Axis < 3; ++Axis)
        {
            if (!Use[Axis])
                continue;
            bValid = bValid && (Neighbor[Axis].Offset != 0);
            Bits &= Neighbor[Axis].FaceBits;
            Offset[Axis] = Neighbor[Axis].Offset;
        }
        if (!bValid)
            continue;

        int Delta = int(countbits(Added & Bits)) - int(countbits(Removed & Bits));
        if (Delta != 0)
            AddBrickCount(Brick + Offset, Delta);
    }
}

[numthreads(4, 4, 4)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    uint3 TexelCount;
    VoxelBits.GetDimensions(TexelCount.x, TexelCount.y, TexelCount.z);

    int3 Texel = RegionMin + int3(DTid);
    if (Texel.z < 0 || Texel.z >= int(TexelCount.z))
        return;

    if (bFullRebuild)
    {
        if (any(Texel >= int3(TexelCount)))
            return;
    }
    else if (!IsDisplacementChanged(Texel))
    {
        return;
    }

    Texel.xy = (Texel.xy + int2(TexelCount.xy)) % int2(TexelCount.xy);

    uint NewBits = ComputeVoxelBits(Texel);
    uint OldBits = VoxelBits[Texel];
    if (NewBits == OldBits)
        return;

    VoxelBits[Texel] = NewBits;
    ApplyBitChange(Texel, OldBits, NewBits);
}