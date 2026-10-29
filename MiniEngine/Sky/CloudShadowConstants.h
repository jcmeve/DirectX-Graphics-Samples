#pragma once
#include <cstdint>
#include "VectorMath.h"

__declspec(align(16)) struct CloudShadowConstants
{
    Math::Matrix4 WorldToShadow;

    Math::Matrix4 ShadowToWorld;

    float SunDirection[3];
    float ShaftMaxDistanceKm;

    uint32_t FrameIndex;
    float Pad[3];
};
