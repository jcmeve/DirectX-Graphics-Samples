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
#include "CompiledShaders/CloudBrickOccupancyCS.h"
//#define BRICK_DISTANCE_FIELD

#ifdef BRICK_DISTANCE_FIELD
#include "CompiledShaders/CloudBrickDistanceCS.h"
#else
#include "CompiledShaders/CloudSectorOccupancyCS.h"
#endif

#include "CompiledShaders/CloudSimulationVelocityCS.h"
#include "CompiledShaders/CloudSimulationAdvectionCS.h"
#include "CompiledShaders/CloudSimulationWriteBackCS.h"
#include "CompiledShaders/CloudVelocityMergeCS.h"
#include "CompiledShaders/CloudLightCacheCS.h"

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
    const float kCloudBoundsSize[3] = { 4096.0f * kCloudScale, 512.0f * kCloudScale, 4096.0f * kCloudScale };

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
#pragma endregion

#pragma region BRICK_OCCUPANCY
    const uint32_t kBrickVoxels = 8;
    const uint32_t kSectorBricks = 4;
    const uint32_t kMaxBrickDistance = 4;

    ComputePSO s_CloudBrickOccupancyPSO(L"Clouds: Brick Occupancy CS");
#ifdef BRICK_DISTANCE_FIELD
    ComputePSO s_CloudBrickDistancePSO(L"Clouds: Brick Distance CS");
#else
    ComputePSO s_CloudSectorOccupancyPSO(L"Clouds: Sector Occupancy CS");
#endif


    VolumeBuffer s_BrickOccupancy;
#ifdef BRICK_DISTANCE_FIELD
    VolumeBuffer s_BrickDistance;
#else
    VolumeBuffer s_SectorOccupancy;
#endif
    uint32_t s_BrickCount[3];
    uint32_t s_DirtyVoxelMin[3];
    uint32_t s_DirtyVoxelMax[3];
    bool s_bOccupancyDirty = true;

#pragma endregion


#pragma region AMORTIZATION
    ColorBuffer s_CloudHistory[2];
    ColorBuffer s_CloudDepthHistory[2];
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
    };

    static const uint32_t kSimulationWindowSize = 64;
    ComputePSO s_CloudSimulationVelocityPSO(L"Clouds: Simulation Velocity CS");
    ComputePSO s_CloudSimulationAdvectionPSO(L"Clouds: Simulation Advection CS");
    ComputePSO s_CloudSimulationWriteBackPSO(L"Clouds: Simulation WriteBack CS");

    BoolVar s_SimulationTrigger("Graphics/Clouds/Simulation/Trigger Explosion", false);
    NumVar s_SimulationEventX("Graphics/Clouds/Simulation/EventX ", 0.5f, 0.0f, 1.0f, 0.01f);
    NumVar s_SimulationEventZ("Graphics/Clouds/Simulation/EventZ ", 0.5f, 0.0f, 1.0f, 0.01f);
    NumVar s_SimulationEventY("Graphics/Clouds/Simulation/EventY ", 0.5f, 0.0f, 1.0f, 0.01f);
    NumVar s_SimulationEventRadius("Graphics/Clouds/Simulation/Radius (m) ", 120.0f, 15.0f, 256.0f, 8.0f);

    NumVar s_SimulationEventStrength("Graphics/Clouds/Simulation/Strength ", 200.0f, 0.0f, 1000.0f, 10.0f);
    NumVar s_SimulationEventDrag("Graphics/Clouds/Simulation/Drag ", 1.0f, 0.0f, 10.0f, 0.1f);
    NumVar s_SimulationEventDuration("Graphics/Clouds/Simulation/Duration ", 8.0f, 1.0f, 30.0f, 1.0f);

    VolumeBuffer s_SimulationVelocity[2];
    VolumeBuffer s_SimulationDensity;
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
    s_RootSig[kSRVs].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 15);
    s_RootSig[kUAVs].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0, 6);
    s_RootSig[kOccupancyConstants].InitAsConstants(5, 4);
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

    s_CloudBrickOccupancyPSO.SetRootSignature(s_RootSig);
    s_CloudBrickOccupancyPSO.SetComputeShader(g_pCloudBrickOccupancyCS, sizeof(g_pCloudBrickOccupancyCS));
    s_CloudBrickOccupancyPSO.Finalize();

#ifdef BRICK_DISTANCE_FIELD

    s_CloudBrickDistancePSO.SetRootSignature(s_RootSig);
    s_CloudBrickDistancePSO.SetComputeShader(g_pCloudBrickDistanceCS, sizeof(g_pCloudBrickDistanceCS));
    s_CloudBrickDistancePSO.Finalize();

#else
    s_CloudSectorOccupancyPSO.SetRootSignature(s_RootSig);
    s_CloudSectorOccupancyPSO.SetComputeShader(g_pCloudSectorOccupancyCS, sizeof(g_pCloudSectorOccupancyCS));
    s_CloudSectorOccupancyPSO.Finalize();

#endif
    s_ShadowMapPSO.SetRootSignature(s_RootSig);
    s_ShadowMapPSO.SetComputeShader(g_pCloudShadowCS, sizeof(g_pCloudShadowCS));
    s_ShadowMapPSO.Finalize();



    s_ShadowMap.Create(L"Cloud Shadow Map", kShadowMapSize, kShadowMapSize, 1, DXGI_FORMAT_R16G16B16A16_FLOAT);
    s_CloudHistory[0].Create(L"CloudHistory 0", Graphics::g_SceneColorBuffer.GetWidth(), Graphics::g_SceneColorBuffer.GetHeight(), 1, DXGI_FORMAT_R16G16B16A16_FLOAT);
    s_CloudHistory[1].Create(L"CloudHistory 1", Graphics::g_SceneColorBuffer.GetWidth(), Graphics::g_SceneColorBuffer.GetHeight(), 1, DXGI_FORMAT_R16G16B16A16_FLOAT);
    s_CloudDepthHistory[0].Create(L"CloudDepthHistory 0", Graphics::g_SceneColorBuffer.GetWidth(), Graphics::g_SceneColorBuffer.GetHeight(), 1, DXGI_FORMAT_R16_FLOAT);
    s_CloudDepthHistory[1].Create(L"CloudDepthHistory 1", Graphics::g_SceneColorBuffer.GetWidth(), Graphics::g_SceneColorBuffer.GetHeight(), 1, DXGI_FORMAT_R16_FLOAT);

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
    const uint32_t VoxelCount[3] = { (uint32_t)ModelingDesc.Width, (uint32_t)ModelingDesc.Height, (uint32_t)ModelingDesc.DepthOrArraySize };
    s_CloudVolume.Create(L"Cloud Volume", VoxelCount[0], VoxelCount[1], VoxelCount[2], ModelingDesc.MipLevels, ModelingDesc.Format);

    GraphicsContext& InitContext = GraphicsContext::Begin(L"Cloud Volume Init");
    InitContext.CopyBuffer(s_CloudVolume, ModelingSource);
    InitContext.TransitionResource(s_CloudVolume, kShaderResourceState);
    InitContext.Finish(true);

    s_CloudVelocity.Create(L"Cloud Velocity", Graphics::g_SceneColorBuffer.GetWidth(), ::g_SceneColorBuffer.GetHeight(), 1, DXGI_FORMAT_R32_UINT);

    s_CloudVelocityMergePSO.SetRootSignature(s_RootSig);
    s_CloudVelocityMergePSO.SetComputeShader(g_pCloudVelocityMergeCS, sizeof(g_pCloudVelocityMergeCS));
    s_CloudVelocityMergePSO.Finalize();

    for (int i = 0; i < 3; ++i)
        s_BrickCount[i] = (VoxelCount[i] + kBrickVoxels - 1) / kBrickVoxels;


    s_BrickOccupancy.Create(L"Cloud Brick Occupancy", s_BrickCount[0], s_BrickCount[1], s_BrickCount[2], 1, DXGI_FORMAT_R32_FLOAT);
#ifdef BRICK_DISTANCE_FIELD
    s_BrickDistance.Create(L"Cloud Sector Occupancy", s_BrickCount[0], s_BrickCount[1], s_BrickCount[2], 1, DXGI_FORMAT_R32_FLOAT);
#else
    uint32_t SectorCount[3];

    for (int i = 0; i < 3; ++i)
    {
        SectorCount[i] = (s_BrickCount[i] + kSectorBricks - 1) / kSectorBricks;
    }
    s_SectorOccupancy.Create(L"Cloud Sector Occupancy", SectorCount[0], SectorCount[1], SectorCount[2], 1, DXGI_FORMAT_R32_FLOAT);

#endif

    const uint32_t VoxelMin[3] = { 0,0,0 };
    SetVolumeDirty(VoxelMin, VoxelCount);
    s_bOccupancyDirty = true;



    s_CloudSimulationVelocityPSO.SetRootSignature(s_RootSig);
    s_CloudSimulationVelocityPSO.SetComputeShader(g_pCloudSimulationVelocityCS, sizeof(g_pCloudSimulationVelocityCS));
    s_CloudSimulationVelocityPSO.Finalize();

    s_CloudSimulationAdvectionPSO.SetRootSignature(s_RootSig);
    s_CloudSimulationAdvectionPSO.SetComputeShader(g_pCloudSimulationAdvectionCS, sizeof(g_pCloudSimulationAdvectionCS));
    s_CloudSimulationAdvectionPSO.Finalize();

    s_CloudSimulationWriteBackPSO.SetRootSignature(s_RootSig);
    s_CloudSimulationWriteBackPSO.SetComputeShader(g_pCloudSimulationWriteBackCS, sizeof(g_pCloudSimulationWriteBackCS));
    s_CloudSimulationWriteBackPSO.Finalize();

    for (int i = 0; i < 3; ++i) {
        s_VoxelCount[i] = VoxelCount[i];
    }
    s_VoxelMeters = kCloudBoundsSize[0] / VoxelCount[0];

    s_SimulationVelocity[0].Create(L"Cloud Simulation Velocity 0", kSimulationWindowSize, kSimulationWindowSize, kSimulationWindowSize, 1, DXGI_FORMAT_R16G16B16A16_FLOAT);
    s_SimulationVelocity[1].Create(L"Cloud Simulation Velocity 1", kSimulationWindowSize, kSimulationWindowSize, kSimulationWindowSize, 1, DXGI_FORMAT_R16G16B16A16_FLOAT);
    s_SimulationDensity.Create(L"Cloud Simulation Density", kSimulationWindowSize, kSimulationWindowSize, kSimulationWindowSize, 1, ModelingDesc.Format);

    s_LightCacheVolumePSO.SetRootSignature(s_RootSig);
    s_LightCacheVolumePSO.SetComputeShader(g_pCloudLightCacheCS, sizeof(g_pCloudLightCacheCS));
    s_LightCacheVolumePSO.Finalize();

    s_LightCacheVolume.Create(L"Cloud Light Cache Volume", VoxelCount[0] / kLightCacheRatio, VoxelCount[1] / kLightCacheRatio, VoxelCount[2] / kLightCacheRatio, 1, DXGI_FORMAT_R32_FLOAT);
}

void Clouds::Shutdown(void)
{
    s_NoiseTexture = nullptr;
    s_CloudVolume.Destroy();
    s_ShadowMap.Destroy();
    s_CloudHistory[0].Destroy();
    s_CloudHistory[1].Destroy();
    s_CloudDepthHistory[0].Destroy();
    s_CloudDepthHistory[1].Destroy();
    s_CloudRenderTarget.Destroy();
    s_CloudDepthRenderTarget.Destroy();
    s_CloudVelocity.Destroy();
    s_BrickOccupancy.Destroy();
    s_LightCacheVolume.Destroy();
   
#ifdef BRICK_DISTANCE_FIELD
    s_BrickDistance.Destroy();
#else
    s_SectorOccupancy.Destroy();
#endif

    s_SimulationVelocity[0].Destroy();
    s_SimulationVelocity[1].Destroy();
    s_SimulationDensity.Destroy();

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
#ifdef BRICK_DISTANCE_FIELD
        Context.TransitionResource(s_BrickDistance, kShaderResourceState);
#else
        Context.TransitionResource(s_SectorOccupancy, kShaderResourceState);
#endif
        Context.TransitionResource(s_LightCacheVolume, kShaderResourceState);

        const D3D12_CPU_DESCRIPTOR_HANDLE RTVs[] = { s_CloudRenderTarget.GetRTV(),s_CloudDepthRenderTarget.GetRTV()};
        Context.SetRenderTargets(2, RTVs);
        Context.SetViewportAndScissor(0, 0, s_CloudRenderTarget.GetWidth(), s_CloudRenderTarget.GetHeight());
        
        Atmosphere::BindSkyInputs(Context, kAtmosphereCBV, kViewCBV, kSRVs);
        Context.SetDynamicDescriptor(kSRVs, 3, g_SceneDepthBuffer.GetDepthSRV());

        Context.SetDynamicConstantBufferView(kCloudCBV, sizeof(Constants), &Constants);
        Context.SetDynamicDescriptor(kSRVs, 4, s_NoiseTexture.GetSRV());
        Context.SetDynamicDescriptor(kSRVs, 5, s_CloudVolume.GetSRV());
        Context.SetDynamicDescriptor(kSRVs, 12, s_BrickOccupancy.GetSRV());

#ifdef BRICK_DISTANCE_FIELD
        Context.SetDynamicDescriptor(kSRVs, 13, s_BrickDistance.GetSRV());
#else
        Context.SetDynamicDescriptor(kSRVs, 13, s_SectorOccupancy.GetSRV());
#endif

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
            Context.TransitionResource(s_CloudDepthHistory[0], kShaderResourceState);
            Context.TransitionResource(s_CloudDepthHistory[1], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
            Context.SetDynamicDescriptor(kSRVs, 8, s_CloudHistory[0].GetSRV());
            Context.SetDynamicDescriptor(kUAVs, 0, s_CloudHistory[1].GetUAV());
            Context.SetDynamicDescriptor(kSRVs, 9, s_CloudDepthHistory[0].GetSRV());
            Context.SetDynamicDescriptor(kUAVs, 1, s_CloudDepthHistory[1].GetUAV());
        }
        else
        {
            Context.TransitionResource(s_CloudHistory[1], kShaderResourceState);
            Context.TransitionResource(s_CloudHistory[0], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
            Context.TransitionResource(s_CloudDepthHistory[1], kShaderResourceState);
            Context.TransitionResource(s_CloudDepthHistory[0], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
            Context.SetDynamicDescriptor(kSRVs, 8, s_CloudHistory[1].GetSRV());
            Context.SetDynamicDescriptor(kUAVs, 0, s_CloudHistory[0].GetUAV());
            Context.SetDynamicDescriptor(kSRVs, 9, s_CloudDepthHistory[1].GetSRV());
            Context.SetDynamicDescriptor(kUAVs, 1, s_CloudDepthHistory[0].GetUAV());
        }

        Context.SetDynamicConstantBufferView(kCloudCBV, sizeof(Constants), &Constants);


        Context.SetDynamicDescriptor(kSRVs, 3, g_SceneDepthBuffer.GetDepthSRV());
        Context.SetDynamicDescriptor(kSRVs, 4, s_NoiseTexture.GetSRV());
        Context.SetDynamicDescriptor(kSRVs, 5, s_CloudVolume.GetSRV());
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
    if (!s_bOccupancyDirty)
        return;
    uint32_t BrickMin[3], BrickMax[3], DistanceMin[3], DistanceMax[3];

    for (int i = 0; i < 3; ++i)
    {
        BrickMin[i] = (s_DirtyVoxelMin[i] > 0 ? s_DirtyVoxelMin[i] - 1 : 0) / kBrickVoxels;
        BrickMax[i] = min(s_DirtyVoxelMax[i] / kBrickVoxels + 1, s_BrickCount[i]);

        DistanceMin[i] = (BrickMin[i] > kMaxBrickDistance) ? BrickMin[i] - kMaxBrickDistance : 0;
        DistanceMax[i] = min(BrickMax[i] + kMaxBrickDistance, s_BrickCount[i]);
    }

    ScopedTimer _prof(L"Cloud Occupancy", Context);

    Context.SetRootSignature(s_RootSig);
    Context.TransitionResource(s_BrickOccupancy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);

#ifdef BRICK_DISTANCE_FIELD

    Context.TransitionResource(s_BrickDistance, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
#else
    Context.TransitionResource(s_SectorOccupancy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
#endif

    Context.SetDynamicDescriptor(kSRVs, 5, s_CloudVolume.GetSRV());
    Context.SetDynamicDescriptor(kUAVs, 0, s_BrickOccupancy.GetUAV());
#ifdef BRICK_DISTANCE_FIELD
    Context.SetDynamicDescriptor(kUAVs, 1, s_BrickDistance.GetUAV());
#else
    Context.SetDynamicDescriptor(kUAVs, 1, s_SectorOccupancy.GetUAV());

#endif
    Context.SetPipelineState(s_CloudBrickOccupancyPSO);
    Context.SetConstants(kOccupancyConstants, BrickMin[0], BrickMin[1], BrickMin[2]);
    Context.Dispatch(BrickMax[0] - BrickMin[0], BrickMax[1] - BrickMin[1], BrickMax[2] - BrickMin[2]);
    Context.InsertUAVBarrier(s_BrickOccupancy);

#ifdef BRICK_DISTANCE_FIELD
    Context.SetPipelineState(s_CloudBrickDistancePSO);
#else
    Context.SetPipelineState(s_CloudSectorOccupancyPSO);
#endif
    Context.SetConstants(kOccupancyConstants, DistanceMin[0], DistanceMin[1], DistanceMin[2]);
    
#ifdef BRICK_DISTANCE_FIELD

    Context.Dispatch3D(DistanceMax[0] - DistanceMin[0], DistanceMax[1] - DistanceMin[1], DistanceMax[2] - DistanceMin[2], 4, 4, 4);
    Context.InsertUAVBarrier(s_BrickDistance);
#else
    uint32_t SectorMin[3], SectorMax[3];
    for (int i = 0; i < 3; ++i)
    {
        SectorMin[i] = BrickMin[i] / kSectorBricks;
        SectorMax[i] = (BrickMax[i] + kSectorBricks - 1) / kSectorBricks;
    }

    Context.SetPipelineState(s_CloudSectorOccupancyPSO);
    Context.SetConstants(kOccupancyConstants, SectorMin[0], SectorMin[1], SectorMin[2]);
    Context.Dispatch3D(SectorMax[0] - SectorMin[0], SectorMax[1] - SectorMin[1], SectorMax[2] - SectorMin[2], 4, 4, 4);
    Context.InsertUAVBarrier(s_SectorOccupancy);
#endif

    Context.TransitionResource(s_BrickOccupancy, kShaderResourceState);

#ifdef BRICK_DISTANCE_FIELD
    Context.TransitionResource(s_BrickDistance, kShaderResourceState);
#else
    Context.TransitionResource(s_SectorOccupancy, kShaderResourceState);
#endif


	s_bOccupancyDirty = false;
}

void Clouds::UpdateSimulation(ComputeContext& Context)
{

    const float DeltaTime = Graphics::GetFrameTime();

    const float WindRadians = s_GlobalWindDirection * Math::XM_PI / 180.0f;
    s_GlobalWindDeltaMeters[0] = cosf(WindRadians) * s_GlobalWindSpeed * DeltaTime;
    s_GlobalWindDeltaMeters[2] = sinf(WindRadians) * s_GlobalWindSpeed * DeltaTime;
    s_GlobalWindOffset[0] = fmodf(s_GlobalWindOffset[0] + s_GlobalWindDeltaMeters[0], kCloudBoundsSize[0]);
    s_GlobalWindOffset[2] = fmodf(s_GlobalWindOffset[2] + s_GlobalWindDeltaMeters[2], kCloudBoundsSize[2]);

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
            const int Origin = (int)s_SimulationEventCenter[i] - (int)kSimulationWindowSize / 2;
            s_SimulationWindowOrigin[i] = (uint32_t)(max(0, min(Origin, (int)(s_VoxelCount[i] - kSimulationWindowSize))));
        }
        s_SimulationTimeLeft = s_SimulationEventDuration;

        s_SimulationTrigger = false;

    }

    if (s_SimulationTimeLeft <= 0.0f)
        return;

    ScopedTimer _prof(L"CLoud Silumation Activated", Context);

    s_SimulationTimeLeft -= DeltaTime;
    
    CloudSimulationConstants Constants = {};
    Constants.DeltaTime = DeltaTime;
    Constants.EventRadius = s_SimulationEventRadius / s_VoxelMeters;
    Constants.EventStrength = s_SimulationEventStrength / s_VoxelMeters;
    Constants.Drag = s_SimulationEventDrag;
    Constants.bEventActive = bEventActive ? 1 : 0;
    Constants.bResetVelocity = bEventActive ? 1 : 0;

    for (int i = 0; i < 3; ++i) {
        Constants.WindowOrigin[i] = s_SimulationWindowOrigin[i];
        Constants.EventCenter[i] = s_SimulationEventCenter[i];
    }

    VolumeBuffer& PrevVelocity = s_SimulationVelocity[(uint32_t)Graphics::GetFrameCount() % 2];
    VolumeBuffer& Velocity = s_SimulationVelocity[((uint32_t)Graphics::GetFrameCount()+1) % 2];

    Context.SetRootSignature(s_RootSig);
    Context.SetDynamicConstantBufferView(kSimulationCBV, sizeof(CloudSimulationConstants), &Constants);

    //Velocity
    Context.TransitionResource(PrevVelocity, kShaderResourceState);
    Context.TransitionResource(Velocity, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
    Context.SetPipelineState(s_CloudSimulationVelocityPSO);
    Context.SetDynamicDescriptor(kSRVs, 6, PrevVelocity.GetSRV());
    Context.SetDynamicDescriptor(kUAVs, 0, Velocity.GetUAV());
    Context.Dispatch3D(kSimulationWindowSize, kSimulationWindowSize, kSimulationWindowSize, 4, 4, 4);

    //Advection
    Context.TransitionResource(Velocity, kShaderResourceState);
    Context.TransitionResource(s_SimulationDensity, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
    Context.SetPipelineState(s_CloudSimulationAdvectionPSO);
    Context.SetDynamicDescriptor(kSRVs, 5, s_CloudVolume.GetSRV());
    Context.SetDynamicDescriptor(kSRVs, 6, Velocity.GetSRV());
    Context.SetDynamicDescriptor(kUAVs, 0, s_SimulationDensity.GetUAV());
    Context.Dispatch3D(kSimulationWindowSize, kSimulationWindowSize, kSimulationWindowSize, 4, 4, 4);

    //WriteBack
    Context.TransitionResource(s_SimulationDensity, kShaderResourceState);
    Context.TransitionResource(s_CloudVolume, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
    Context.SetPipelineState(s_CloudSimulationWriteBackPSO);
    Context.SetDynamicDescriptor(kSRVs, 6, s_SimulationDensity.GetSRV());
    Context.SetDynamicDescriptor(kUAVs, 0, s_CloudVolume.GetUAV());
    Context.Dispatch3D(kSimulationWindowSize, kSimulationWindowSize, kSimulationWindowSize, 4, 4, 4);

    Context.TransitionResource(s_CloudVolume, kShaderResourceState);

    const uint32_t WindowMax[3] = {
        s_SimulationWindowOrigin[0] + kSimulationWindowSize,
        s_SimulationWindowOrigin[1] + kSimulationWindowSize ,
        s_SimulationWindowOrigin[2] + kSimulationWindowSize };

    SetVolumeDirty(s_SimulationWindowOrigin, WindowMax);

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

void Clouds::SetVolumeDirty(const uint32_t VoxelMin[3], const uint32_t VoxelMax[3])
{
    for (int i = 0; i < 3; ++i) {
        s_DirtyVoxelMin[i] = s_bOccupancyDirty ? min(s_DirtyVoxelMin[i], VoxelMin[i]) : VoxelMin[i];
        s_DirtyVoxelMax[i] = s_bOccupancyDirty ? max(s_DirtyVoxelMax[i],VoxelMax[i]) : VoxelMax[i];
    }
    s_bOccupancyDirty = true;
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
            Context.SetDynamicDescriptor(kSRVs, 12, s_BrickOccupancy.GetSRV());
#ifdef BRICK_DISTANCE_FIELD   
            Context.SetDynamicDescriptor(kSRVs, 13, s_BrickDistance.GetSRV());
#else
            Context.SetDynamicDescriptor(kSRVs, 13, s_SectorOccupancy.GetSRV());
#endif
            Context.SetDynamicDescriptor(kUAVs, 0, s_ShadowMap.GetUAV());
            Context.Dispatch2D(kShadowMapSize, kShadowMapSize, 8, 8);
        }
        Context.TransitionResource(s_ShadowMap, kShaderResourceState);
    }
    if (s_EnableClouds && s_bTexturesValid)
    {
        ScopedTimer _prof2(L"Cloud Light Cache", Context);

        Context.TransitionResource(s_LightCacheVolume, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
        
        Context.SetRootSignature(s_RootSig);
        Context.SetPipelineState(s_LightCacheVolumePSO);

        Context.SetDynamicConstantBufferView(kCloudCBV, sizeof(s_CloudConstants), &s_CloudConstants);
        Context.SetDynamicConstantBufferView(kCloudShadowCBV, sizeof(s_ShadowConstants), &s_ShadowConstants);
        Context.SetDynamicDescriptor(kSRVs, 5, s_CloudVolume.GetSRV());
        Context.SetDynamicDescriptor(kSRVs, 12, s_BrickOccupancy.GetSRV());
#ifdef BRICK_DISTANCE_FIELD   
        Context.SetDynamicDescriptor(kSRVs, 13, s_BrickDistance.GetSRV());
#else
        Context.SetDynamicDescriptor(kSRVs, 13, s_SectorOccupancy.GetSRV());
#endif
        Context.SetDynamicDescriptor(kUAVs, 0, s_LightCacheVolume.GetUAV());
        Context.Dispatch3D(s_VoxelCount[0] / kLightCacheRatio, s_VoxelCount[1] / kLightCacheRatio, s_VoxelCount[2] / kLightCacheRatio, 4, 4, 4);

        Context.TransitionResource(s_LightCacheVolume, kShaderResourceState);
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
