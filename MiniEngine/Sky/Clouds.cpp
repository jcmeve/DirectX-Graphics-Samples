#include "Clouds.h"
#include "RootSignature.h"
#include "PipelineState.h"
#include "CommandContext.h"
#include "GraphicsCommon.h"
#include "BufferManager.h"
#include "TextureManager.h"
#include "EngineTuning.h"
#include "EngineProfiling.h"
#include "Camera.h"
#include "Atmosphere.h"
#include "ColorBuffer.h"
#include "VolumeBuffer.h"
#include <cfloat>
#include <Display.h>

#include "CompiledShaders/CloudAmortizationPS.h"
#include "CompiledShaders/CloudReprojectionPS.h"
#include "CompiledShaders/SkyFullscreenVS.h"
#include "CompiledShaders/SkyDebugViewVolumePS.h"
#include "CompiledShaders/CloudShadowCS.h"

#include "CompiledShaders/CloudVelocityMergeCS.h"
#include "CompiledShaders/CloudLightCacheCS.h"

#include "CompiledShaders/CloudSimulationVelocityCS.h"
#include "CompiledShaders/CloudSimulationAdvectionCS.h"
#include "CompiledShaders/CloudSimulationWriteBackCS.h"
#include "CompiledShaders/CloudSimulationDivergenceCS.h"
#include "CompiledShaders/CloudSimulationJacobiCS.h"
#include "CompiledShaders/CloudSimulationPoissonCS.h"
#include "CompiledShaders/CloudSimulationProjectCS.h"
#include "CompiledShaders/CloudDisplacementClearCS.h"
#include "CompiledShaders/CloudOccupancyUpdateCS.h"
#include "CompiledShaders/CloudOccupancyClearCS.h"

using namespace Graphics;

namespace
{
    enum RootBindings
    {
        kAtmosphereCBV,     //b0
        kDebugConstants,    //b1
        kViewCBV,           //b2
        kCloudCBV,          //b3
        kCloudShadowCBV,    //b4
        kSRVs,              //t0 ~ t13
        kUAVs,              //u0 ~ u5
        kOccupancyConstants,//b5
        kSimulationCBV,      //b6
        kNumRootBindings
    };

    const uint32_t kDebugViewSize = 384;
    const uint32_t kDebugViewMargin = 16;

    const wchar_t* kNoisePath = L"NubisVoxelCloudsPack/DDS/NubisNoise.dds";
    const wchar_t* kModelingPath = L"NubisVoxelCloudsPack/DDS/StormbirdCloud_Modeling.dds";


    RootSignature s_RootSig;
    GraphicsPSO s_DebugViewVolumePSO(L"Clouds: Debug View Volume PSO");



#pragma region CLOUD_DEFAULT
    __declspec(align(16)) struct CloudConstants
    {
        float BoundsMin[3];
        float Extinction;

        float BoundsSize[3];
        float StepScale;
        
        float CameraPosition[3];
        float WorldToMeters;
        
        float AmbientScale;
        float MipDistanceScale;
        uint32_t MaxSteps;
        uint32_t FrameIndex;

        float GlobalWindOffset[3];
        float CloudPad;

        float GlobalWindDeltaMeters[3];
        float CloudPad2;
    };

    const float kWorldUnitsToMeters = 0.01f; //cm to m
    const float kCloudScale = 1.0f;
    const uint32_t kVolumeHeightScale = 4;
    const float kCloudBoundsSize[3] = { 4096.0f * kCloudScale, 512.0f * kVolumeHeightScale * kCloudScale, 4096.0f * kCloudScale };

    const uint32_t kShadowMapSize = 1024;
    const D3D12_RESOURCE_STATES kShaderResourceState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    const uint32_t kLightCacheRatio = 2;

    ColorBuffer s_ShadowMap(Color(0.0f, 0.0f, 0.0f, 0.0f));
    CloudShadowConstants s_ShadowConstants = {};
    CloudConstants s_CloudConstants = {};

    VolumeBuffer s_LightCacheVolume;

    TextureRef s_NoiseTexture; //128*128*128 노이즈 RGBA 각각
    VolumeBuffer s_CloudVolume; //R: Dimensional Profile, G: Detail Type, B: Density Scale 512*512*64  
    bool s_bTexturesValid = false;

    ColorBuffer s_CloudVelocity;
    ComputePSO s_CloudVelocityMergePSO(L"Clouds: Velocity Merge CS");
    ComputePSO s_ShadowMapPSO(L"Clouds: Shadow Map CS");
    ComputePSO s_LightCacheVolumePSO(L"Clouds: Light Cache Volume CS");
    ComputePSO s_CloudDisplacementClearPSO(L"Clouds: Displacement Clear CS");

    BoolVar s_EnableClouds("Graphics/Clouds/Enable", true);
    NumVar s_CloudBaseAltitude("Graphics/Clouds/Base Altitude (m)", 2000.0f, 0.0f, 8000.0f, 100.0f);
    NumVar s_CloudOffsetX("Graphics/Clouds/Offset X (m)", 0.0f, -20000.0f, 20000.0f, 100.0f);
    NumVar s_CloudOffsetZ("Graphics/Clouds/Offset Z (m)", 0.0f, -20000.0f, 20000.0f, 100.0f);
    NumVar s_CloudExtinction("Graphics/Clouds/Extinction (1 per m)", 0.05f, 0.001f, 0.5f, 0.005f);
    NumVar s_CloudStepScale("Graphics/Clouds/Step Scale", 0.12f, 0.01f, 1.0f, 0.01f);
    IntVar s_CloudMaxSteps("Graphics/Clouds/Max Steps", 256, 16, 1024, 16);
    NumVar s_CloudAmbientScale("Graphics/Clouds/Ambient Scale", 1.0f, 0.0f, 4.0f, 0.1f);
    NumVar s_CloudMipDistanceScale("Graphics/Clouds/Noise Mip Distance Scale", 0.004f, 0.0f, 0.05f, 0.001f);
    BoolVar s_EnableCloudShadow("Graphics/Clouds/Shadow/Enable", true);
    NumVar s_CloudShaftDistanceKm("Graphics/Clouds/Shadow/Shaft Distance (km)", 16.0f, 0.0f, 64.0f, 1.0f);

    NumVar s_GlobalWindSpeed("Graphics/Clouds/Wind/Speed (m per s)", 0.0f, 0.0f, 100.0f, 10.0f);
    NumVar s_GlobalWindDirection("Graphics/Clouds/Wind/Direction (deg)", 90.0f, 0.0f, 360.0f, 5.0f);
    float s_GlobalWindOffset[3] = {};
    float s_GlobalWindDeltaMeters[3] = {};

    IntVar s_LightCacheFrames("Graphics/Clouds/Light Cache Frames", 8, 1, 32, 1);
    const float kLightCacheSunEpsilon = 1e-5;

    bool s_bLightCacheFull = true;
    bool s_bLightCacheDirty = false;
    uint32_t s_LightCacheFramesLeft = 0;
    uint32_t s_LightCachePhase = 0;
    float s_LightCacheSunDirection[3] = {};

#pragma endregion

#pragma region BRICK_OCCUPANCY
    const uint32_t kBrickVoxels = 8;
    const uint32_t kSectorBricks = 4;
    const uint32_t kMaxBrickDistance = 4;

    ComputePSO s_CloudOccupancyUpdatePSO(L"Clouds: Occupancy Update CS");
    ComputePSO s_CloudOccupancyClearPSO(L"Clouds: Occupancy Clear CS");

    VolumeBuffer s_BrickOccupancy;
    VolumeBuffer s_SectorOccupancy;

    VolumeBuffer s_VoxelOccupancyBits;
    VolumeBuffer s_SimulationChangeMask;
    VolumeBuffer s_CommitedDisplacement;
    const uint32_t kOccupancyBitBlock[3] = { 4, 4, 2 };//32바이트 비트마스크 & 64복셀
    bool s_bOccupancyRebuild = true;
    bool s_bOccupancyWindowDirty = false;

    uint32_t s_BrickCount[3];

    struct OccupancyConstants
    {
        int32_t RegionMin[3];
        uint32_t bFullRebuild;
        int32_t WindowCellMin[3];
        uint32_t Pad;
    };

    int FloorDiv(int Value, int Divisor)
    {
        return (Value >= 0 ? Value : Value - Divisor + 1) / Divisor;
    }

    void ClearOccupancyVolume(ComputeContext& Context, VolumeBuffer& Target)
    {
        Context.SetPipelineState(s_CloudOccupancyClearPSO);
        Context.SetDynamicDescriptor(kUAVs, 0, Target.GetUAV());
        Context.Dispatch3D(Target.GetWidth(), Target.GetHeight(), Target.GetDepth(), 4, 4, 4);
        Context.InsertUAVBarrier(Target);
    }

#pragma endregion


#pragma region AMORTIZATION
    ColorBuffer s_CloudHistory[2];
    ColorBuffer s_CloudRenderTarget;
    ColorBuffer s_CloudDepthRenderTarget;

    const uint32_t kAmortizationSize = 4;//한 축

    GraphicsPSO s_CloudReprojectionPSO(L"Clouds: Reprojection PSO");
    GraphicsPSO s_CloudAmortizationPSO(L"Clouds: Amortization PSO");
#pragma endregion

#pragma region SIMULATION
    __declspec(align(16)) struct CloudSimulationConstants
    {
        uint32_t WindowOrigin[3];
        float DeltaTime;

        float EventCenter[3];
        float EventRadius;

        float EventStrength;
        float Drag;
        uint32_t bEventActive;
        uint32_t bResetVelocity;

        float EventHeat;
        float Buoyancy;//부력
        float Cooling;
        uint32_t EventType;

        float EventDirection[3];
        float EventSpeed;

        float EventHalfLength;
        float EventInjcetion;
        float SimulationPad[2];

    };

    static const uint32_t kSimulationWindowSize = 64*2;
    static const uint32_t kSimulationCellVoxels = 2;
    static const uint32_t kSimulationWindowVoxels = kSimulationWindowSize*kSimulationCellVoxels;

    ComputePSO s_CloudSimulationVelocityPSO(L"Clouds: Simulation Velocity CS");
    ComputePSO s_CloudSimulationAdvectionPSO(L"Clouds: Simulation Advection CS");
    ComputePSO s_CloudSimulationWriteBackPSO(L"Clouds: Simulation WriteBack CS");
    ComputePSO s_CloudSimulationDivergencePSO(L"Clouds: Simulation Divergence CS");
    ComputePSO s_CloudSimulationJacobiPSO(L"Clouds: Simulation Jacobi CS");
    ComputePSO s_CloudSimulationPoissonPSO(L"Clouds: Simulation Poisson CS");
    ComputePSO s_CloudSimulationProjectPSO(L"Clouds: Simulation Project CS");

    BoolVar s_SimulationUseDST("Graphics/Clouds/Simulation/Use DST", true);


    enum SimulationEventType{kEventExplosion, kEventWind, kEventVortex};
    const char* kSimulationEventLabels[] = { "Explosion", "Wind", "Vortex" };
    static const float kInjectionFadeRatio = 0.3f;

    BoolVar s_SimulationTrigger("Graphics/Clouds/Simulation/Trigger Event", false);
    BoolVar s_SimulationReset("Graphics/Clouds/Simulation/Reset Displacement", false);
    EnumVar s_SimulationEventType("Graphics/Clouds/Simulation/Event Type", kEventExplosion, _countof(kSimulationEventLabels), kSimulationEventLabels);

    NumVar s_SimulationInjectionTime("Graphics/Clouds/Simulation/Injection Time (s) ", 0.3f, 0.05f, 10.0f, 0.05f);
    NumVar s_SimulationEventHeat("Graphics/Clouds/Simulation/Heat ", 1.0f, -10.0f, 10.0f, 0.1f);
    NumVar s_SimulationEventSpeed("Graphics/Clouds/Simulation/Speed(m per s)", 30.0f, 0.0f, 2000.0f, 2.0f);
    NumVar s_SimulationEventLength("Graphics/Clouds/Simulation/Length(m)", 600.0f, 0.0f, 896.0f, 16.0f);
    NumVar s_SimulationEventYaw("Graphics/Clouds/Simulation/Yaw (degree)", 0.0f, -180.0f, 180.0f, 5.0f);
    NumVar s_SimulationEventPitch("Graphics/Clouds/Simulation/Pitch (degree)", 0.0f, -90.0f, 90.0f, 5.0f);


    NumVar s_SimulationEventX("Graphics/Clouds/Simulation/EventX ", 0.5f, 0.0f, 1.0f, 0.01f);
    NumVar s_SimulationEventZ("Graphics/Clouds/Simulation/EventZ ", 0.5f, 0.0f, 1.0f, 0.01f);
    NumVar s_SimulationEventY("Graphics/Clouds/Simulation/EventY ", 0.13f, 0.0f, 1.0f, 0.01f);
    NumVar s_SimulationEventRadius("Graphics/Clouds/Simulation/Radius (m) ", 120.0f, 15.0f, 1024.0f*4, 8.0f);

    NumVar s_SimulationEventStrength("Graphics/Clouds/Simulation/Expansion Rate (1 per s) ", 5.0f, 0.0f, 500.0, 0.5f);
    IntVar s_SimulationJacobiIterations("Graphics/Clouds/Simulation/Jacobi Iteractions", 30, 2, 80, 2);
    NumVar s_SimulationEventDrag("Graphics/Clouds/Simulation/Drag ", 0.2f, 0.0f, 10.0f, 0.1f);
    NumVar s_SimulationEventDuration("Graphics/Clouds/Simulation/Duration ", 8.0f, 1.0f, 30.0f, 1.0f);

    NumVar s_SimulationEventBuoyancy("Graphics/Clouds/Simulation/Buoyancy (m per s2)", 40.0f, 0.0f, 400.0f, 4.0f);
    NumVar s_SimulationEventCooling("Graphics/Clouds/Simulation/Cooling ", 0.7f, 0.0f, 5.0f, 0.05f);


    VolumeBuffer s_SimulationDivergence;
    VolumeBuffer s_SimulationVelocity[2];
    VolumeBuffer s_SimulationPressure[2];

    VolumeBuffer s_CloudDisplacement;
    VolumeBuffer s_SimulationDisplacement;

    uint32_t s_VoxelCount[3];
    float s_VoxelMeters;
    uint32_t s_SimulationWindowOrigin[3];
    float s_SimulationEventCenter[3];
    float s_SimulationTimeLeft = 0.0f;



#pragma endregion

    enum DebugVolume
    {
        kDebugVolumeNone,
        kDebugVolumeNoise,
        kDebugVolumeModeling,
    };
    const char* kDebugVolumeLabels[] = { "None", "Noise", "Modeling"};

    EnumVar s_DebugVolume("Graphics/Clouds/Debug/Show Volume", kDebugVolumeNone, _countof(kDebugVolumeLabels), kDebugVolumeLabels);
    NumVar s_DebugVolumeSlice("Graphics/Clouds/Debug/Volume Slice", 0.45f, 0.0f, 1.0f, 0.01f);

    const D3D12_CPU_DESCRIPTOR_HANDLE GetDebugSRV(void)
    {
        switch (s_DebugVolume)
        {
        case kDebugVolumeNoise:    return s_NoiseTexture.GetSRV();
        case kDebugVolumeModeling: return s_CloudVolume.GetSRV();
        default:                   return s_CloudVolume.GetSRV();
        }
    }

    void GetCloudBoundsMin(float(&BoundsMin)[3])
    {
        BoundsMin[0] = s_CloudOffsetX - kCloudBoundsSize[0] * 0.5f;
        BoundsMin[1] = s_CloudBaseAltitude;
        BoundsMin[2] = s_CloudOffsetZ - kCloudBoundsSize[2] * 0.5f;
    }


    CloudConstants BuildCloudConstants(const Math::Camera& Camera)
    {
        Math::Vector3 CameraPos = Camera.GetPosition() * kWorldUnitsToMeters;
        const float Camera3[3] = { (float)CameraPos.GetX(), (float)CameraPos.GetY(), (float)CameraPos.GetZ() };
        float BoundsMin[3];
        GetCloudBoundsMin(BoundsMin);

        CloudConstants Constants = {};
        for (int Axis = 0; Axis < 3; ++Axis)
        {
            Constants.BoundsMin[Axis] = BoundsMin[Axis] - Camera3[Axis];
            Constants.BoundsSize[Axis] = kCloudBoundsSize[Axis];
            Constants.CameraPosition[Axis] = Camera3[Axis];

            Constants.GlobalWindOffset[Axis] = s_GlobalWindOffset[Axis];
            Constants.GlobalWindDeltaMeters[Axis] = s_GlobalWindDeltaMeters[Axis];
        }
        Constants.Extinction = s_CloudExtinction;
        Constants.StepScale = s_CloudStepScale;
        Constants.WorldToMeters = kWorldUnitsToMeters;
        Constants.AmbientScale = s_CloudAmbientScale;
        Constants.MipDistanceScale = s_CloudMipDistanceScale;
        Constants.MaxSteps = (uint32_t)(int)s_CloudMaxSteps;
        Constants.FrameIndex = (uint32_t)Graphics::GetFrameCount();
        return Constants;
    }

    CloudShadowConstants BuildShadowConstants(const Math::Vector3& SunDirection)
    {
        using namespace Math;

        float BoundsMin[3];
        GetCloudBoundsMin(BoundsMin);
        const Vector3 BoxMin(BoundsMin[0], BoundsMin[1], BoundsMin[2]);
        const Vector3 BoxSize(kCloudBoundsSize[0], kCloudBoundsSize[1], kCloudBoundsSize[2]);
        const Vector3 Center = BoxMin + BoxSize * 0.5f;
        const float Radius = (float)Length(BoxSize) * 0.5f;

        // 광원 공간 basis
        const Vector3 Sun = Normalize(SunDirection);
        const Vector3 RefUp = (fabsf((float)Sun.GetY()) < 0.99f) ? Vector3(kYUnitVector) : Vector3(kXUnitVector);
        const Vector3 Right = Normalize(Cross(RefUp, Sun));
        const Vector3 Up = Cross(Sun, Right);
        const Vector3 Origin = Center + Sun * Radius;

        // 박스 꼭짓점 8개를 광원 평면에 투영해 맵 영역 결정
        float RectMin[2] = { FLT_MAX, FLT_MAX };
        float RectMax[2] = { -FLT_MAX, -FLT_MAX };
        for (int Corner = 0; Corner < 8; ++Corner)
        {
            const Vector3 Offset((float)(Corner & 1), (float)((Corner >> 1) & 1), (float)((Corner >> 2) & 1));
            const Vector3 ToCorner = BoxMin + BoxSize * Offset - Origin;
            const float Projected[2] = { (float)Dot(ToCorner, Right), (float)Dot(ToCorner, Up) };
            for (int Axis = 0; Axis < 2; ++Axis)
            {
                RectMin[Axis] = Math::Min(RectMin[Axis], Projected[Axis]);
                RectMax[Axis] = Math::Max(RectMax[Axis], Projected[Axis]);
            }
        }
        const float ScaleU = 1.0f / (RectMax[0] - RectMin[0]);
        const float ScaleV = 1.0f / (RectMax[1] - RectMin[1]);

        const Matrix4 Rows(
            Vector4(Right * ScaleU, -((float)Dot(Origin, Right) + RectMin[0]) * ScaleU),
            Vector4(Up * ScaleV, -((float)Dot(Origin, Up) + RectMin[1]) * ScaleV),
            Vector4(-Sun, (float)Dot(Origin, Sun)),
            Vector4(0.0f, 0.0f, 0.0f, 1.0f));

        CloudShadowConstants Constants = {};
        Constants.WorldToShadow = Transpose(Rows);
        Constants.ShadowToWorld = Invert(Constants.WorldToShadow);
        Constants.SunDirection[0] = (float)Sun.GetX();
        Constants.SunDirection[1] = (float)Sun.GetY();
        Constants.SunDirection[2] = (float)Sun.GetZ();
        Constants.ShaftMaxDistanceKm = s_CloudShaftDistanceKm;
        Constants.FrameIndex = (uint32_t)Graphics::GetFrameCount();
        return Constants;
    }
}

void Clouds::Initialize(void)
{
    // s0 = 디버그 뷰용 Clamp, s1 = 타일링 노이즈 조회용 Wrap
    s_RootSig.Reset(kNumRootBindings, 2);
    s_RootSig.InitStaticSampler(0, SamplerLinearClampDesc);
    s_RootSig.InitStaticSampler(1, SamplerLinearWrapDesc);
    s_RootSig[kAtmosphereCBV].InitAsConstantBuffer(0);
    s_RootSig[kDebugConstants].InitAsConstants(1, 4);
    s_RootSig[kViewCBV].InitAsConstantBuffer(2);
    s_RootSig[kCloudCBV].InitAsConstantBuffer(3);
    s_RootSig[kCloudShadowCBV].InitAsConstantBuffer(4);
    s_RootSig[kSRVs].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 16);
    s_RootSig[kUAVs].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0, 6);
    s_RootSig[kOccupancyConstants].InitAsConstants(5, 8);
    s_RootSig[kSimulationCBV].InitAsConstantBuffer(6);
    s_RootSig.Finalize(L"Clouds");

    s_DebugViewVolumePSO.SetRootSignature(s_RootSig);
    s_DebugViewVolumePSO.SetRasterizerState(RasterizerTwoSided);
    s_DebugViewVolumePSO.SetBlendState(BlendDisable);
    s_DebugViewVolumePSO.SetDepthStencilState(DepthStateDisabled);
    s_DebugViewVolumePSO.SetInputLayout(0, nullptr);
    s_DebugViewVolumePSO.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
    s_DebugViewVolumePSO.SetRenderTargetFormat(g_OverlayBuffer.GetFormat(), DXGI_FORMAT_UNKNOWN);
    s_DebugViewVolumePSO.SetVertexShader(g_pSkyFullscreenVS, sizeof(g_pSkyFullscreenVS));
    s_DebugViewVolumePSO.SetPixelShader(g_pSkyDebugViewVolumePS, sizeof(g_pSkyDebugViewVolumePS));
    s_DebugViewVolumePSO.Finalize();

    s_CloudReprojectionPSO.SetRootSignature(s_RootSig);
    s_CloudReprojectionPSO.SetRasterizerState(RasterizerTwoSided);
    s_CloudReprojectionPSO.SetBlendState(BlendPreMultiplied);
    s_CloudReprojectionPSO.SetDepthStencilState(DepthStateDisabled);
    s_CloudReprojectionPSO.SetInputLayout(0, nullptr);
    s_CloudReprojectionPSO.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
    s_CloudReprojectionPSO.SetRenderTargetFormat(g_SceneColorBuffer.GetFormat(), DXGI_FORMAT_UNKNOWN);
    s_CloudReprojectionPSO.SetVertexShader(g_pSkyFullscreenVS, sizeof(g_pSkyFullscreenVS));
    s_CloudReprojectionPSO.SetPixelShader(g_pCloudReprojectionPS, sizeof(g_pCloudReprojectionPS));
    s_CloudReprojectionPSO.Finalize();

    s_CloudAmortizationPSO.SetRootSignature(s_RootSig);
    s_CloudAmortizationPSO.SetRasterizerState(RasterizerTwoSided);
    s_CloudAmortizationPSO.SetBlendState(BlendDisable);
    s_CloudAmortizationPSO.SetDepthStencilState(DepthStateDisabled);
    s_CloudAmortizationPSO.SetInputLayout(0, nullptr);
    s_CloudAmortizationPSO.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
    const DXGI_FORMAT AmortizationFormat[] = { DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16_FLOAT };
    s_CloudAmortizationPSO.SetRenderTargetFormats(2, AmortizationFormat, DXGI_FORMAT_UNKNOWN);
    s_CloudAmortizationPSO.SetVertexShader(g_pSkyFullscreenVS, sizeof(g_pSkyFullscreenVS));
    s_CloudAmortizationPSO.SetPixelShader(g_pCloudAmortizationPS, sizeof(g_pCloudAmortizationPS));
    s_CloudAmortizationPSO.Finalize(); 

    s_CloudOccupancyUpdatePSO.SetRootSignature(s_RootSig);
    s_CloudOccupancyUpdatePSO.SetComputeShader(g_pCloudOccupancyUpdateCS, sizeof(g_pCloudOccupancyUpdateCS));
    s_CloudOccupancyUpdatePSO.Finalize();

    s_CloudOccupancyClearPSO.SetRootSignature(s_RootSig);
    s_CloudOccupancyClearPSO.SetComputeShader(g_pCloudOccupancyClearCS, sizeof(g_pCloudOccupancyClearCS));
    s_CloudOccupancyClearPSO.Finalize();



    s_ShadowMapPSO.SetRootSignature(s_RootSig);
    s_ShadowMapPSO.SetComputeShader(g_pCloudShadowCS, sizeof(g_pCloudShadowCS));
    s_ShadowMapPSO.Finalize();



    s_ShadowMap.Create(L"Cloud Shadow Map", kShadowMapSize, kShadowMapSize, 1, DXGI_FORMAT_R16G16B16A16_FLOAT);
    s_CloudHistory[0].Create(L"CloudHistory 0", Graphics::g_SceneColorBuffer.GetWidth(), Graphics::g_SceneColorBuffer.GetHeight(), 1, DXGI_FORMAT_R16G16B16A16_FLOAT);
    s_CloudHistory[1].Create(L"CloudHistory 1", Graphics::g_SceneColorBuffer.GetWidth(), Graphics::g_SceneColorBuffer.GetHeight(), 1, DXGI_FORMAT_R16G16B16A16_FLOAT);

    const uint32_t CloudRenderTargetWidth = (Graphics::g_SceneColorBuffer.GetWidth() + kAmortizationSize - 1) / kAmortizationSize;
    const uint32_t CloudRenderTargetHeight = (Graphics::g_SceneColorBuffer.GetHeight() + kAmortizationSize - 1) / kAmortizationSize;
    s_CloudRenderTarget.Create(L"CloudRenderTarget", CloudRenderTargetWidth, CloudRenderTargetHeight, 1, DXGI_FORMAT_R16G16B16A16_FLOAT);
    s_CloudDepthRenderTarget.Create(L"CloudDepthRenderTarget", CloudRenderTargetWidth, CloudRenderTargetHeight, 1, DXGI_FORMAT_R16_FLOAT);

    s_NoiseTexture = TextureManager::LoadDDSFromFile(kNoisePath);
    TextureRef ModelingTexture = TextureManager::LoadDDSFromFile(kModelingPath);
    s_bTexturesValid = s_NoiseTexture.IsValid() && ModelingTexture.IsValid();
    ASSERT(s_bTexturesValid);

    Texture& ModelingSource = const_cast<Texture&> (*ModelingTexture.Get());
    const D3D12_RESOURCE_DESC ModelingDesc = ModelingSource.GetResource()->GetDesc();
    const uint32_t VoxelCount[3] = { (uint32_t)ModelingDesc.Width, (uint32_t)ModelingDesc.Height, (uint32_t)ModelingDesc.DepthOrArraySize * kVolumeHeightScale };
    s_CloudVolume.Create(L"Cloud Volume", VoxelCount[0], VoxelCount[1], VoxelCount[2], 1, ModelingDesc.Format);
    s_CloudDisplacement.Create(L"Cloud Displacement", VoxelCount[0]/kSimulationCellVoxels, VoxelCount[1] / kSimulationCellVoxels, VoxelCount[2] / kSimulationCellVoxels, 1, DXGI_FORMAT_R16G16B16A16_FLOAT);

    GraphicsContext& InitContext = GraphicsContext::Begin(L"Cloud Volume Init");
    InitContext.TransitionResource(s_CloudVolume, D3D12_RESOURCE_STATE_COPY_DEST);
    InitContext.TransitionResource(s_CloudDisplacement, kShaderResourceState);
    InitContext.TransitionResource(ModelingSource, D3D12_RESOURCE_STATE_COPY_SOURCE, true);

    D3D12_TEXTURE_COPY_LOCATION Dest = {};
    Dest.pResource = s_CloudVolume.GetResource();
    Dest.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    Dest.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION Source = {};
    Source.pResource = ModelingSource.GetResource();
    Source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    Source.SubresourceIndex = 0;

    const D3D12_BOX SourceBox = { ModelingDesc.Width / 2 - 32 , ModelingDesc.Height / 2 - 32,0,ModelingDesc.Width / 2 + 32 , ModelingDesc.Height / 2 + 32, (UINT)ModelingDesc.DepthOrArraySize };
    for (int i = 0; i < ModelingDesc.Width / 64; ++i) {
        for (int j = 0; j < ModelingDesc.Height / 64; ++j) {
            InitContext.GetCommandList()->CopyTextureRegion(&Dest, i*64, j*64, 16, &Source, &SourceBox);
        }
    }

    InitContext.TransitionResource(s_CloudVolume, kShaderResourceState);
    InitContext.Finish(true);

    s_CloudVelocity.Create(L"Cloud Velocity", Graphics::g_SceneColorBuffer.GetWidth(), ::g_SceneColorBuffer.GetHeight(), 1, DXGI_FORMAT_R32_UINT);

    s_CloudVelocityMergePSO.SetRootSignature(s_RootSig);
    s_CloudVelocityMergePSO.SetComputeShader(g_pCloudVelocityMergeCS, sizeof(g_pCloudVelocityMergeCS));
    s_CloudVelocityMergePSO.Finalize();

    for (int i = 0; i < 3; ++i)
        s_BrickCount[i] = (VoxelCount[i] + kBrickVoxels - 1) / kBrickVoxels;


    s_BrickOccupancy.Create(L"Cloud Brick Occupancy", s_BrickCount[0], s_BrickCount[1], s_BrickCount[2], 1, DXGI_FORMAT_R32_FLOAT);

    uint32_t SectorCount[3];

    for (int i = 0; i < 3; ++i)
    {
        SectorCount[i] = (s_BrickCount[i] + kSectorBricks - 1) / kSectorBricks;
    }
    s_SectorOccupancy.Create(L"Cloud Sector Occupancy", SectorCount[0], SectorCount[1], SectorCount[2], 1, DXGI_FORMAT_R32_FLOAT);

    s_BrickOccupancy.Create(L"Cloud Brick Occupancy", s_BrickCount[0], s_BrickCount[1], s_BrickCount[2], 1, DXGI_FORMAT_R32_UINT);
    s_SectorOccupancy.Create(L"Cloud Sector Occupancy", SectorCount[0], SectorCount[1], SectorCount[2], 1, DXGI_FORMAT_R32_UINT);

    s_VoxelOccupancyBits.Create(L"Cloud Voxel Occupancy Bits", VoxelCount[0] / kOccupancyBitBlock[0], VoxelCount[1] / kOccupancyBitBlock[1], VoxelCount[2] / kOccupancyBitBlock[2], 1, DXGI_FORMAT_R32_UINT);
    s_SimulationChangeMask.Create(L"Cloud Simulation Change Mask", kSimulationWindowSize, kSimulationWindowSize, kSimulationWindowSize, 1, DXGI_FORMAT_R8_UINT);
    s_CommitedDisplacement.Create(L"Cloud Commited Displacement", VoxelCount[0] / kSimulationCellVoxels, VoxelCount[1] / kSimulationCellVoxels, VoxelCount[2] / kSimulationCellVoxels, 1, DXGI_FORMAT_R16G16B16A16_FLOAT);


    s_bOccupancyRebuild = true;
    s_bLightCacheDirty = true;

    s_CloudSimulationVelocityPSO.SetRootSignature(s_RootSig);
    s_CloudSimulationVelocityPSO.SetComputeShader(g_pCloudSimulationVelocityCS, sizeof(g_pCloudSimulationVelocityCS));
    s_CloudSimulationVelocityPSO.Finalize();

    s_CloudSimulationAdvectionPSO.SetRootSignature(s_RootSig);
    s_CloudSimulationAdvectionPSO.SetComputeShader(g_pCloudSimulationAdvectionCS, sizeof(g_pCloudSimulationAdvectionCS));
    s_CloudSimulationAdvectionPSO.Finalize();

    s_CloudSimulationWriteBackPSO.SetRootSignature(s_RootSig);
    s_CloudSimulationWriteBackPSO.SetComputeShader(g_pCloudSimulationWriteBackCS, sizeof(g_pCloudSimulationWriteBackCS));
    s_CloudSimulationWriteBackPSO.Finalize();

    s_CloudSimulationDivergencePSO.SetRootSignature(s_RootSig);
    s_CloudSimulationDivergencePSO.SetComputeShader(g_pCloudSimulationDivergenceCS, sizeof(g_pCloudSimulationDivergenceCS));
    s_CloudSimulationDivergencePSO.Finalize();

    s_CloudSimulationJacobiPSO.SetRootSignature(s_RootSig);
    s_CloudSimulationJacobiPSO.SetComputeShader(g_pCloudSimulationJacobiCS, sizeof(g_pCloudSimulationJacobiCS));
    s_CloudSimulationJacobiPSO.Finalize();

    s_CloudSimulationPoissonPSO.SetRootSignature(s_RootSig);
    s_CloudSimulationPoissonPSO.SetComputeShader(g_pCloudSimulationPoissonCS, sizeof(g_pCloudSimulationPoissonCS));
    s_CloudSimulationPoissonPSO.Finalize();

    s_CloudSimulationProjectPSO.SetRootSignature(s_RootSig);
    s_CloudSimulationProjectPSO.SetComputeShader(g_pCloudSimulationProjectCS, sizeof(g_pCloudSimulationProjectCS));
    s_CloudSimulationProjectPSO.Finalize();

    for (int i = 0; i < 3; ++i) {
        s_VoxelCount[i] = VoxelCount[i];
    }
    s_VoxelMeters = kCloudBoundsSize[0] / VoxelCount[0];

    s_SimulationVelocity[0].Create(L"Cloud Simulation Velocity 0", kSimulationWindowSize, kSimulationWindowSize, kSimulationWindowSize, 1, DXGI_FORMAT_R16G16B16A16_FLOAT);
    s_SimulationVelocity[1].Create(L"Cloud Simulation Velocity 1", kSimulationWindowSize, kSimulationWindowSize, kSimulationWindowSize, 1, DXGI_FORMAT_R16G16B16A16_FLOAT);
    s_SimulationDisplacement.Create(L"Cloud Simulation Displacement", kSimulationWindowSize, kSimulationWindowSize, kSimulationWindowSize, 1, DXGI_FORMAT_R16G16B16A16_FLOAT);
    s_SimulationPressure[0].Create(L"Cloud Simulation Pressure 0", kSimulationWindowSize, kSimulationWindowSize, kSimulationWindowSize, 1, DXGI_FORMAT_R32_FLOAT);
    s_SimulationPressure[1].Create(L"Cloud Simulation Pressure 1", kSimulationWindowSize, kSimulationWindowSize, kSimulationWindowSize, 1, DXGI_FORMAT_R32_FLOAT);
    s_SimulationDivergence.Create(L"Cloud Simulation Divergence", kSimulationWindowSize, kSimulationWindowSize, kSimulationWindowSize, 1, DXGI_FORMAT_R32_FLOAT);

    s_LightCacheVolumePSO.SetRootSignature(s_RootSig);
    s_LightCacheVolumePSO.SetComputeShader(g_pCloudLightCacheCS, sizeof(g_pCloudLightCacheCS));
    s_LightCacheVolumePSO.Finalize();

    s_LightCacheVolume.Create(L"Cloud Light Cache Volume", VoxelCount[0] / kLightCacheRatio, VoxelCount[1] / kLightCacheRatio, VoxelCount[2] / kLightCacheRatio, 1, DXGI_FORMAT_R32_FLOAT);

    s_CloudDisplacementClearPSO.SetRootSignature(s_RootSig);
    s_CloudDisplacementClearPSO.SetComputeShader(g_pCloudDisplacementClearCS, sizeof(g_pCloudDisplacementClearCS));
    s_CloudDisplacementClearPSO.Finalize();
}

void Clouds::Shutdown(void)
{
    s_NoiseTexture = nullptr;
    s_CloudVolume.Destroy();
    s_ShadowMap.Destroy();
    s_CloudHistory[0].Destroy();
    s_CloudHistory[1].Destroy();
    s_CloudRenderTarget.Destroy();
    s_CloudDepthRenderTarget.Destroy();
    s_CloudVelocity.Destroy();
    s_BrickOccupancy.Destroy();
    s_SectorOccupancy.Destroy();
    s_VoxelOccupancyBits.Destroy();
    s_SimulationChangeMask.Destroy();
    s_CommitedDisplacement.Destroy();
    s_LightCacheVolume.Destroy();
    s_SimulationVelocity[0].Destroy();
    s_SimulationVelocity[1].Destroy();
    s_CloudDisplacement.Destroy();
    s_SimulationDisplacement.Destroy();
    s_SimulationPressure[0].Destroy();
    s_SimulationPressure[1].Destroy();
    s_SimulationDivergence.Destroy();
}

void Clouds::Render(GraphicsContext& Context, const Math::Camera& Camera, const D3D12_VIEWPORT& Viewport, const D3D12_RECT& Scissor)
{
    if (!s_EnableClouds || !s_bTexturesValid)
        return;

    ScopedTimer _prof(L"Clouds", Context);

    CloudConstants Constants = BuildCloudConstants(Camera);
    
    {
        ScopedTimer _prof2(L"Amortization PS", Context);

        //Context.ClearColor(s_CloudRenderTarget);
        //Context.ClearColor(s_CloudDepthRenderTarget);

        Context.SetRootSignature(s_RootSig);
        Context.SetPipelineState(s_CloudAmortizationPSO);
        Context.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        Context.TransitionResource(g_SceneDepthBuffer, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Context.TransitionResource(s_CloudRenderTarget, D3D12_RESOURCE_STATE_RENDER_TARGET, true);
        Context.TransitionResource(s_CloudDepthRenderTarget, D3D12_RESOURCE_STATE_RENDER_TARGET, true);
        Context.TransitionResource(s_BrickOccupancy, kShaderResourceState);
        Context.TransitionResource(s_SectorOccupancy, kShaderResourceState);
        Context.TransitionResource(s_LightCacheVolume, kShaderResourceState);

        const D3D12_CPU_DESCRIPTOR_HANDLE RTVs[] = { s_CloudRenderTarget.GetRTV(),s_CloudDepthRenderTarget.GetRTV()};
        Context.SetRenderTargets(2, RTVs);
        Context.SetViewportAndScissor(0, 0, s_CloudRenderTarget.GetWidth(), s_CloudRenderTarget.GetHeight());
        
        Atmosphere::BindSkyInputs(Context, kAtmosphereCBV, kViewCBV, kSRVs);
        Context.SetDynamicDescriptor(kSRVs, 3, g_SceneDepthBuffer.GetDepthSRV());

        Context.SetDynamicConstantBufferView(kCloudCBV, sizeof(Constants), &Constants);
        Context.SetDynamicDescriptor(kSRVs, 4, s_NoiseTexture.GetSRV());
        Context.SetDynamicDescriptor(kSRVs, 5, s_CloudVolume.GetSRV());
        Context.SetDynamicDescriptor(kSRVs, 15, s_CloudDisplacement.GetSRV());
        Context.SetDynamicDescriptor(kSRVs, 12, s_BrickOccupancy.GetSRV());
        Context.SetDynamicDescriptor(kSRVs, 13, s_SectorOccupancy.GetSRV());
        Context.SetDynamicDescriptor(kSRVs, 14, s_LightCacheVolume.GetSRV());

        Context.Draw(3);
    }
    {
        Context.SetRootSignature(s_RootSig);
        Context.SetPipelineState(s_CloudReprojectionPSO);
        Context.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        Context.TransitionResource(g_SceneDepthBuffer, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Context.TransitionResource(g_SceneColorBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, true);
        Context.SetRenderTarget(g_SceneColorBuffer.GetRTV());
        Context.SetViewportAndScissor(Viewport, Scissor);

        ScopedTimer _prof3(L"Reprojection PS", Context);
        // t0~t2 Atmosphere에 바인딩 위임
        Atmosphere::BindSkyInputs(Context, kAtmosphereCBV, kViewCBV, kSRVs);
        Context.TransitionResource(s_CloudRenderTarget, kShaderResourceState);
        Context.TransitionResource(s_CloudDepthRenderTarget, kShaderResourceState);
        Context.TransitionResource(s_CloudVelocity, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
        Context.SetDynamicDescriptor(kSRVs, 10, s_CloudRenderTarget.GetSRV());
        Context.SetDynamicDescriptor(kSRVs, 11, s_CloudDepthRenderTarget.GetSRV());
        Context.SetDynamicDescriptor(kUAVs, 2, s_CloudVelocity.GetUAV());
        if ((uint32_t)Graphics::GetFrameCount() % 2)
        {
            Context.TransitionResource(s_CloudHistory[0], kShaderResourceState);
            Context.TransitionResource(s_CloudHistory[1], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
            Context.SetDynamicDescriptor(kSRVs, 8, s_CloudHistory[0].GetSRV());
            Context.SetDynamicDescriptor(kUAVs, 0, s_CloudHistory[1].GetUAV());
        }
        else
        {
            Context.TransitionResource(s_CloudHistory[1], kShaderResourceState);
            Context.TransitionResource(s_CloudHistory[0], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
            Context.SetDynamicDescriptor(kSRVs, 8, s_CloudHistory[1].GetSRV());
            Context.SetDynamicDescriptor(kUAVs, 0, s_CloudHistory[0].GetUAV());
        }

        Context.SetDynamicConstantBufferView(kCloudCBV, sizeof(Constants), &Constants);


        Context.SetDynamicDescriptor(kSRVs, 3, g_SceneDepthBuffer.GetDepthSRV());
        Context.SetDynamicDescriptor(kSRVs, 4, s_NoiseTexture.GetSRV());
        Context.SetDynamicDescriptor(kSRVs, 5, s_CloudVolume.GetSRV());
        Context.SetDynamicDescriptor(kSRVs, 15, s_CloudDisplacement.GetSRV());
        Context.Draw(3);
    }
}

void Clouds::RenderDebug(GraphicsContext& Context)
{
    if (s_DebugVolume == kDebugVolumeNone || !s_bTexturesValid)
        return;

    Context.SetRootSignature(s_RootSig);
    Context.SetPipelineState(s_DebugViewVolumePSO);
    Context.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    Context.SetRenderTarget(g_OverlayBuffer.GetRTV());
    Context.SetDynamicDescriptor(kSRVs, 0, s_CloudVolume.GetSRV());
    Context.SetDynamicDescriptor(kSRVs, 0, GetDebugSRV());
    Context.SetConstants(kDebugConstants, 1.0f, (float)s_DebugVolumeSlice);

    Context.SetViewportAndScissor(g_OverlayBuffer.GetWidth() - kDebugViewSize - kDebugViewMargin, kDebugViewMargin,
        kDebugViewSize, kDebugViewSize);
    Context.Draw(3);
}

void Clouds::UpdateOccupancy(ComputeContext& Context)
{
    if (!s_bOccupancyRebuild && !s_bOccupancyWindowDirty)
        return;

    ScopedTimer _prof(L"Cloud Occupancy", Context);
    Context.SetRootSignature(s_RootSig);
    Context.TransitionResource(s_SimulationChangeMask, kShaderResourceState);
    Context.TransitionResource(s_VoxelOccupancyBits, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Context.TransitionResource(s_BrickOccupancy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Context.TransitionResource(s_SectorOccupancy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);

    OccupancyConstants Constants = {};
    uint32_t TexelCount[3];
    if (s_bOccupancyRebuild)
    {
        ClearOccupancyVolume(Context, s_VoxelOccupancyBits);
        ClearOccupancyVolume(Context, s_BrickOccupancy);
        ClearOccupancyVolume(Context, s_SectorOccupancy);
        Constants.bFullRebuild = 1;
        for (int i = 0; i < 3; ++i)
            TexelCount[i] = s_VoxelCount[i] / kOccupancyBitBlock[i];
        Context.CopyBuffer(s_CommitedDisplacement, s_CloudDisplacement);
    }
    else
    {
        for (int i = 0; i < 3; ++i)
        {
            const int Origin = (int)s_SimulationWindowOrigin[i];
            const int Block = (int)kOccupancyBitBlock[i];
            Constants.RegionMin[i] = FloorDiv(Origin - 1, Block);
            TexelCount[i] = (uint32_t)(FloorDiv(Origin + (int)kSimulationWindowVoxels, Block) + 1 - Constants.RegionMin[i]);
            Constants.WindowCellMin[i] = Origin / (int)kSimulationCellVoxels;
        }
    }

    Context.SetPipelineState(s_CloudOccupancyUpdatePSO);
    Context.SetConstantArray(kOccupancyConstants, 8, &Constants);
    Context.SetDynamicDescriptor(kSRVs, 5, s_CloudVolume.GetSRV());
    Context.SetDynamicDescriptor(kSRVs, 9, s_SimulationChangeMask.GetSRV());
    Context.SetDynamicDescriptor(kSRVs, 15, s_CloudDisplacement.GetSRV());
    Context.SetDynamicDescriptor(kUAVs, 0, s_VoxelOccupancyBits.GetUAV());
    Context.SetDynamicDescriptor(kUAVs, 1, s_BrickOccupancy.GetUAV());
    Context.SetDynamicDescriptor(kUAVs, 2, s_SectorOccupancy.GetUAV());
    Context.Dispatch3D(TexelCount[0], TexelCount[1], TexelCount[2], 4, 4, 4);

    Context.TransitionResource(s_BrickOccupancy, kShaderResourceState);
    Context.TransitionResource(s_SectorOccupancy, kShaderResourceState);

    s_bOccupancyRebuild = false;
    s_bOccupancyWindowDirty = false;
}

void Clouds::UpdateSimulation(ComputeContext& Context)
{
    const float DeltaTime = Graphics::GetFrameTime();

    const float WindRadians = s_GlobalWindDirection * Math::XM_PI / 180.0f;
    s_GlobalWindDeltaMeters[0] = cosf(WindRadians) * s_GlobalWindSpeed * DeltaTime;
    s_GlobalWindDeltaMeters[2] = sinf(WindRadians) * s_GlobalWindSpeed * DeltaTime;
    s_GlobalWindOffset[0] = fmodf(s_GlobalWindOffset[0] + s_GlobalWindDeltaMeters[0], kCloudBoundsSize[0]);
    s_GlobalWindOffset[2] = fmodf(s_GlobalWindOffset[2] + s_GlobalWindDeltaMeters[2], kCloudBoundsSize[2]);

    if (s_SimulationReset) {
        s_SimulationReset = false;
        s_SimulationTimeLeft = 0.0f;

        Context.SetRootSignature(s_RootSig);
        Context.TransitionResource(s_CloudDisplacement, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
        Context.SetPipelineState(s_CloudDisplacementClearPSO);
        Context.SetDynamicDescriptor(kUAVs, 0, s_CloudDisplacement.GetUAV());
        Context.Dispatch3D(s_VoxelCount[0] / kSimulationCellVoxels, s_VoxelCount[1] / kSimulationCellVoxels, s_VoxelCount[2] / kSimulationCellVoxels, 4, 4, 4);
        Context.TransitionResource(s_CloudDisplacement, kShaderResourceState);

        const uint32_t VoxelMin[3] = { 0,0,0 };
        s_bOccupancyRebuild = true;
        s_bLightCacheDirty = true;
    }

    const bool bEventActive = s_SimulationTrigger;
    if (bEventActive)
    {
        float EventNorm[3] = {
            s_SimulationEventX - s_GlobalWindOffset[0] / kCloudBoundsSize[0],
            s_SimulationEventZ - s_GlobalWindOffset[2] / kCloudBoundsSize[2],
            s_SimulationEventY };
        EventNorm[0] -= floorf(EventNorm[0]);
        EventNorm[1] -= floorf(EventNorm[1]);

        for(int i = 0; i < 3; ++i)
        {
            s_SimulationEventCenter[i] = EventNorm[i] * s_VoxelCount[i];
            const int Origin = ((int)s_SimulationEventCenter[i] - (int)kSimulationWindowVoxels / 2) & ~(int)(kSimulationCellVoxels - 1);
            s_SimulationWindowOrigin[i] = (uint32_t)(max(0, min(Origin, (int)(s_VoxelCount[i] - kSimulationWindowVoxels))));
        }
        s_SimulationTimeLeft = s_SimulationEventDuration;

        s_SimulationTrigger = false;

    }

    if (s_SimulationTimeLeft <= 0.0f)
        return;

    ScopedTimer _prof(L"Cloud Silumation Activated", Context);

    const float Elapsed = s_SimulationEventDuration - s_SimulationTimeLeft;
    const float FadeStart = s_SimulationInjectionTime * (1.0f - kInjectionFadeRatio);
    const float FadeT = max(0.0f, min(1.0f, (Elapsed - FadeStart) / (s_SimulationInjectionTime - FadeStart)));
    const float Injection = 1.0f - FadeT * FadeT * (3.0f - 2.0f * FadeT);
    s_SimulationTimeLeft -= DeltaTime;
    
    const bool bExplosion = s_SimulationEventType == kEventExplosion;
    const float CellMeters = s_VoxelMeters * kSimulationCellVoxels;
    const float Yaw = s_SimulationEventYaw * Math::XM_PI / 180.0f;
    const float Pitch = s_SimulationEventPitch * Math::XM_PI / 180.0f;

    CloudSimulationConstants Constants = {};
    Constants.DeltaTime = DeltaTime;
    Constants.EventRadius = s_SimulationEventRadius / s_VoxelMeters;
    Constants.EventStrength = bExplosion ? (float)s_SimulationEventStrength : 0.0f;
    Constants.EventInjcetion = Injection;
    Constants.EventType = (uint32_t)s_SimulationEventType;
    Constants.EventSpeed = s_SimulationEventSpeed / CellMeters;
    Constants.EventHalfLength = bExplosion ? 0.0f : 0.5f * s_SimulationEventLength / s_VoxelMeters;

    Constants.EventDirection[0] = cosf(Pitch) * cosf(Yaw);
    Constants.EventDirection[1] = cosf(Pitch) * sinf(Yaw);
    Constants.EventDirection[2] = sinf(Pitch);


    Constants.Drag = s_SimulationEventDrag;
    Constants.bEventActive = bEventActive ? 1 : 0;
    Constants.bResetVelocity = bEventActive ? 1 : 0;
    Constants.EventHeat = s_SimulationEventHeat;
    Constants.Buoyancy = s_SimulationEventBuoyancy/(s_VoxelMeters*kSimulationCellVoxels);
    Constants.Cooling = s_SimulationEventCooling;

    for (int i = 0; i < 3; ++i) {
        Constants.WindowOrigin[i] = s_SimulationWindowOrigin[i];
        Constants.EventCenter[i] = s_SimulationEventCenter[i];
    }

    VolumeBuffer& PrevVelocity = s_SimulationVelocity[(uint32_t)Graphics::GetFrameCount() % 2];
    VolumeBuffer& Velocity = s_SimulationVelocity[((uint32_t)Graphics::GetFrameCount()+1) % 2];

    Context.SetRootSignature(s_RootSig);
    Context.SetDynamicConstantBufferView(kSimulationCBV, sizeof(CloudSimulationConstants), &Constants);
    {
        ScopedTimer _prof1(L"Cloud Silumation Velocity", Context);

        //Velocity
        Context.TransitionResource(PrevVelocity, kShaderResourceState);
        Context.TransitionResource(Velocity, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
        Context.SetPipelineState(s_CloudSimulationVelocityPSO);
        Context.SetDynamicDescriptor(kSRVs, 6, PrevVelocity.GetSRV());
        Context.SetDynamicDescriptor(kUAVs, 0, Velocity.GetUAV());
        Context.Dispatch3D(kSimulationWindowSize, kSimulationWindowSize, kSimulationWindowSize, 4, 4, 4);
    }
    {
        ScopedTimer _prof2(L"Cloud Silumation Divergence", Context);
        //Divergence
        Context.TransitionResource(Velocity, kShaderResourceState);
        Context.TransitionResource(s_SimulationDivergence, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
        Context.TransitionResource(s_SimulationPressure[0], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
        Context.SetPipelineState(s_CloudSimulationDivergencePSO);
        Context.SetDynamicDescriptor(kSRVs, 6, Velocity.GetSRV());
        Context.SetDynamicDescriptor(kUAVs, 0, s_SimulationDivergence.GetUAV());
        Context.SetDynamicDescriptor(kUAVs, 1, s_SimulationPressure[0].GetUAV());
        Context.Dispatch3D(kSimulationWindowSize, kSimulationWindowSize, kSimulationWindowSize, 4, 4, 4);
    }
    {
        ScopedTimer _prof3(L"Cloud Silumation Pressure", Context);
        //Jacobi
        Context.TransitionResource(s_SimulationDivergence, kShaderResourceState);
        Context.SetDynamicDescriptor(kSRVs, 7, s_SimulationDivergence.GetSRV());

        if (s_SimulationUseDST) {
            static const uint32_t kPassAxis[5] = { 0,1,2,1,0 };
            Context.SetPipelineState(s_CloudSimulationPoissonPSO);
            Context.TransitionResource(s_SimulationPressure[0], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
            Context.SetDynamicDescriptor(kUAVs, 0, s_SimulationPressure[0].GetUAV());
            for (uint32_t Pass = 0; Pass < 5; ++Pass) {
                Context.SetConstants(kOccupancyConstants, kPassAxis[Pass], Pass == 2 ? 1u : 0u, Pass == 0 ? 1u : 0u);
                Context.Dispatch(kSimulationWindowSize/2, kSimulationWindowSize, 1);
                Context.InsertUAVBarrier(s_SimulationPressure[0]);
            }
        }
        else {
            Context.SetPipelineState(s_CloudSimulationJacobiPSO);
            const int JacobiIterations = (s_SimulationJacobiIterations + 1) & ~1;
            for (int i = 0; i < JacobiIterations; ++i) {
                VolumeBuffer& Src = s_SimulationPressure[i % 2];
                VolumeBuffer& Dst = s_SimulationPressure[(i + 1) % 2];
                Context.TransitionResource(Src, kShaderResourceState);
                Context.TransitionResource(Dst, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
                Context.SetDynamicDescriptor(kSRVs, 8, Src.GetSRV());
                Context.SetDynamicDescriptor(kUAVs, 0, Dst.GetUAV());
                Context.Dispatch3D(kSimulationWindowSize, kSimulationWindowSize, kSimulationWindowSize, 4, 4, 4);

            }
        }
    }
    {
        ScopedTimer _prof4(L"Cloud Silumation Project", Context);
        //Project
        Context.TransitionResource(s_SimulationPressure[0], kShaderResourceState);
        Context.TransitionResource(Velocity, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
        Context.SetPipelineState(s_CloudSimulationProjectPSO);
        Context.SetDynamicDescriptor(kSRVs, 8, s_SimulationPressure[0].GetSRV());
        Context.SetDynamicDescriptor(kUAVs, 0, Velocity.GetUAV());
        Context.Dispatch3D(kSimulationWindowSize, kSimulationWindowSize, kSimulationWindowSize, 4, 4, 4);
    }

    {
        ScopedTimer _prof4(L"Cloud Silumation Displacement", Context);
        //Advection
        Context.TransitionResource(Velocity, kShaderResourceState);
        Context.TransitionResource(s_SimulationDisplacement, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
        Context.TransitionResource(s_SimulationChangeMask, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
        Context.TransitionResource(s_CommitedDisplacement, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);

        Context.SetPipelineState(s_CloudSimulationAdvectionPSO);
        Context.SetDynamicDescriptor(kSRVs, 6, Velocity.GetSRV());
        Context.SetDynamicDescriptor(kSRVs, 15, s_CloudDisplacement.GetSRV());
        Context.SetDynamicDescriptor(kUAVs, 0, s_SimulationDisplacement.GetUAV());
        Context.SetDynamicDescriptor(kUAVs, 1, s_SimulationChangeMask.GetUAV());
        Context.SetDynamicDescriptor(kUAVs, 2, s_CommitedDisplacement.GetUAV());
        Context.Dispatch3D(kSimulationWindowSize, kSimulationWindowSize, kSimulationWindowSize, 4, 4, 4);
    }

    {
        ScopedTimer _prof5(L"Cloud Silumation WriteBack", Context);
        //WriteBack
        Context.TransitionResource(s_SimulationDisplacement, kShaderResourceState);
        Context.TransitionResource(s_CloudDisplacement, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
        Context.SetPipelineState(s_CloudSimulationWriteBackPSO);
        Context.SetDynamicDescriptor(kSRVs, 6, s_SimulationDisplacement.GetSRV());
        Context.SetDynamicDescriptor(kUAVs, 0, s_CloudDisplacement.GetUAV());
        Context.Dispatch3D(kSimulationWindowSize, kSimulationWindowSize, kSimulationWindowSize, 4, 4, 4);
    }
    Context.TransitionResource(s_CloudDisplacement, kShaderResourceState);

    const uint32_t WindowMax[3] = {
        s_SimulationWindowOrigin[0] + kSimulationWindowVoxels,
        s_SimulationWindowOrigin[1] + kSimulationWindowVoxels,
        s_SimulationWindowOrigin[2] + kSimulationWindowVoxels };

    s_bOccupancyWindowDirty = true;
    s_bLightCacheDirty = true;
}

void Clouds::WriteVelocity(ComputeContext& Context)
{
    if (!s_EnableClouds || !s_bTexturesValid)
        return;
    
    ScopedTimer _prof(L"Cloud Velocity", Context);

    Context.SetRootSignature(s_RootSig);
    Context.SetPipelineState(s_CloudVelocityMergePSO);

    Context.TransitionResource(s_CloudVelocity, kShaderResourceState);
    Context.TransitionResource(g_VelocityBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
    Context.SetDynamicDescriptor(kSRVs, 0, s_CloudVelocity.GetSRV());
    Context.SetDynamicDescriptor(kUAVs, 0, g_VelocityBuffer.GetUAV());
    Context.Dispatch2D(s_CloudVelocity.GetWidth(), s_CloudVelocity.GetHeight(), 8, 8);
}

void Clouds::RenderShadowMapAndLightCache(ComputeContext& Context, const Math::Camera& Camera, const Math::Vector3& SunDirection)
{

    s_ShadowConstants = BuildShadowConstants(SunDirection);
    s_CloudConstants = BuildCloudConstants(Camera);
    {
        ScopedTimer _prof(L"Cloud Shadow Map", Context);
        
        Context.TransitionResource(s_ShadowMap, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
        if (!s_EnableClouds || !s_EnableCloudShadow || !s_bTexturesValid)
        {
            Context.ClearUAV(s_ShadowMap);
            s_ShadowConstants.ShaftMaxDistanceKm = 0.0f;
        }
        else
        {
            Context.SetRootSignature(s_RootSig);
            Context.SetPipelineState(s_ShadowMapPSO);
            Context.SetDynamicConstantBufferView(kCloudCBV, sizeof(s_CloudConstants), &s_CloudConstants);
            Context.SetDynamicConstantBufferView(kCloudShadowCBV, sizeof(s_ShadowConstants), &s_ShadowConstants);
            Context.SetDynamicDescriptor(kSRVs, 4, s_NoiseTexture.GetSRV());
            Context.SetDynamicDescriptor(kSRVs, 5, s_CloudVolume.GetSRV());
            Context.SetDynamicDescriptor(kSRVs, 15, s_CloudDisplacement.GetSRV());
            Context.SetDynamicDescriptor(kSRVs, 12, s_BrickOccupancy.GetSRV());
            Context.SetDynamicDescriptor(kSRVs, 13, s_SectorOccupancy.GetSRV());
            Context.SetDynamicDescriptor(kUAVs, 0, s_ShadowMap.GetUAV());
            Context.Dispatch2D(kShadowMapSize, kShadowMapSize, 8, 8);
        }
        Context.TransitionResource(s_ShadowMap, kShaderResourceState);
    }
    if (s_EnableClouds && s_bTexturesValid)
    {
        ScopedTimer _prof2(L"Cloud Light Cache", Context);
        const float SunDot =
            s_ShadowConstants.SunDirection[0] * s_LightCacheSunDirection[0] 
            + s_ShadowConstants.SunDirection[1] * s_LightCacheSunDirection[1] 
            + s_ShadowConstants.SunDirection[2] * s_LightCacheSunDirection[2];
        if (1.0f-SunDot > kLightCacheSunEpsilon || s_bLightCacheDirty) {
            for (int i = 0; i < 3; ++i) {
                s_LightCacheSunDirection[i] = s_ShadowConstants.SunDirection[i];
            }
            s_LightCacheFramesLeft = s_LightCacheFrames;
            s_bLightCacheDirty = false;
        }

        if (s_bLightCacheFull || s_LightCacheFramesLeft > 0) {

            const uint32_t Stride = s_bLightCacheFull ? 1 : s_LightCacheFrames;
            const uint32_t Phase = s_bLightCacheFull ? 0 : s_LightCachePhase % s_LightCacheFrames;

            Context.TransitionResource(s_LightCacheVolume, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);

            Context.SetRootSignature(s_RootSig);
            Context.SetPipelineState(s_LightCacheVolumePSO);

            Context.SetDynamicConstantBufferView(kCloudCBV, sizeof(s_CloudConstants), &s_CloudConstants);
            Context.SetDynamicConstantBufferView(kCloudShadowCBV, sizeof(s_ShadowConstants), &s_ShadowConstants);
            Context.SetConstants(kOccupancyConstants, Stride, Phase);

            Context.SetDynamicDescriptor(kSRVs, 5, s_CloudVolume.GetSRV());
            Context.SetDynamicDescriptor(kSRVs, 15, s_CloudDisplacement.GetSRV());
            Context.SetDynamicDescriptor(kSRVs, 12, s_BrickOccupancy.GetSRV());
            Context.SetDynamicDescriptor(kSRVs, 13, s_SectorOccupancy.GetSRV());
            Context.SetDynamicDescriptor(kUAVs, 0, s_LightCacheVolume.GetUAV());
            Context.Dispatch3D(s_VoxelCount[0] / kLightCacheRatio, s_VoxelCount[1] / kLightCacheRatio, (s_VoxelCount[2] / kLightCacheRatio + Stride - 1) / Stride, 4, 4, 4);

            Context.TransitionResource(s_LightCacheVolume, kShaderResourceState);

            if (s_bLightCacheFull) 
            {
                s_bLightCacheFull = false;
                s_LightCacheFramesLeft = 0;
            }
            else 
            {
                ++s_LightCachePhase;
                --s_LightCacheFramesLeft;
            }
        }


    }
}

D3D12_CPU_DESCRIPTOR_HANDLE Clouds::GetShadowMapSRV(void)
{
    return s_ShadowMap.GetSRV();
}

const CloudShadowConstants& Clouds::GetShadowConstants(void)
{
    return s_ShadowConstants;
}
