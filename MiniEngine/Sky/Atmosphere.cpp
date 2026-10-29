#include "Atmosphere.h"
#include "RootSignature.h"
#include "PipelineState.h"
#include "CommandContext.h"
#include "ColorBuffer.h"
#include "VolumeBuffer.h"
#include "GraphicsCommon.h"
#include "BufferManager.h"
#include "EngineTuning.h"
#include "EngineProfiling.h"
#include "Camera.h"
#include "TextureManager.h"
#include "CloudShadowConstants.h"

#include "CompiledShaders/AtmosphereTransmittanceLutCS.h"
#include "CompiledShaders/SkyFullscreenVS.h"
#include "CompiledShaders/SkyDebugViewPS.h"
#include "CompiledShaders/SkyDebugViewVolumePS.h"
#include "CompiledShaders/AtmosphereMultiScatteringLutCS.h"
#include "CompiledShaders/AtmosphereRaymarchingReferencePS.h"
#include "CompiledShaders/AtmosphereSkyViewLutCS.h"
#include "CompiledShaders/AtmosphereSkyViewPS.h"
#include "CompiledShaders/AtmosphereAerialPerspectiveCS.h"
#include "CompiledShaders/AtmosphereAerialPerspectivePS.h"

using namespace Graphics;

namespace
{
    //AtmosphereCommon.hlsli
    __declspec(align(16)) struct AtmosphereConstants
    {
        float RayleighScattering[3];    
        float RayleighScaleHeight;

        float MieScattering[3];         
        float MieScaleHeight;

        float MieExtinction[3];//Scattering + absorption   
        float MiePhaseG;

        float OzoneExtinction[3];       
        float OzoneCenterHeight;

        float GroundAlbedo[3];          
        float OzoneHalfWidth;

        float BottomRadius;
        float TopRadius;
        float Pad[2];
    };

    __declspec(align(16)) struct SkyViewConstants
    {
        Math::Matrix4 InvViewProj;

        float CameraPositionKm[3];      
        float SunIlluminance;

        float SunDirection[3];          
        float SunDiskCosAngle;

        float SunDiskIntensity;         
        float MultiScatteringFactor;
        float AerialPerspectiveWorldToKm;
        float Pad;
    };

    enum RootBindings
    {
        kAtmosphereCBV,     //b0
        kDebugConstants,    //b1
        kViewCBV,           //b2
        kCloudShadowCBV,    //b4
        kSRVs,              //t0 ~ t7
        kUAVs,              //u0 ~ u3
        kNumRootBindings
    };

    const uint32_t kTransmittanceLutWidth = 256;
    const uint32_t kTransmittanceLutHeight = 64;
    const uint32_t kMultiScatteringLutSize = 32;
    const uint32_t kMultiScatteringDirectionCount = 64;
    const uint32_t kSkyViewLutWidth = 192;
    const uint32_t kSkyViewLutHeight = 108;
    const uint32_t kVolumeTestSize = 32;
    const uint32_t kDebugViewScale = 3;
    const uint32_t kDebugViewMargin = 16;
    const uint32_t kAerialPerspectiveSize = 32;

    enum SkyMode
    {
        kSkyModeReference,
        kSkyModeSkyViewLut,
    };
    const char* kSkyModeLabels[] = { "Reference", "Sky View LUT" };
    
    const D3D12_RESOURCE_STATES kShaderResourceState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const float kWorldUnitsToKm = 1.0f / 100000.0f; //cm to km
    const float kSunDiskHalfAngleRadians = 0.5f * 0.53f * Math::XM_PI / 180.0f;



    RootSignature s_RootSig;
    ComputePSO s_TransmittanceLutPSO(L"Atmosphere: Transmittance LUT CS");
    ComputePSO s_MultiScatteringLutPSO(L"Atmosphere: Multi Scattering LUT CS");
    GraphicsPSO s_DebugViewPSO(L"Atmosphere: Debug View PSO");
    GraphicsPSO s_DebugViewVolumePSO(L"Atmosphere: Debug View Volume PSO");
    GraphicsPSO s_AtmosphereRaymarchingReferencePSO(L"Atmosphere: Raymarching PSO");
    ComputePSO s_SkyViewLutPSO(L"Atmosphere: Sky View LUT CS");
    GraphicsPSO s_SkyViewCompositePSO(L"Atmosphere: Sky View Composite PSO");
    ComputePSO s_AerialPerspectivePSO(L"Atmosphere: Aerial Perspective CS");
    GraphicsPSO s_ApplyAerialPerspectivePSO(L"Atmosphere: Apply Aerial Perspective PSO");


    ColorBuffer s_TransmittanceLut;
    ColorBuffer s_MultiScatteringLut;
    ColorBuffer s_SkyViewLut;
    VolumeBuffer s_AerialPerspectiveVolume;

    AtmosphereConstants s_Constants;
    SkyViewConstants s_LastViewConstants;

    CloudShadowConstants s_CloudShadowConstants;
    D3D12_CPU_DESCRIPTOR_HANDLE s_CloudShadowSRV;

    bool s_bTransmittanceDirty = true;
    bool s_bVolumeTestDirty = true;

    BoolVar s_ForceUpdateLuts("Graphics/Atmosphere/Force Update LUTs", false);
    BoolVar s_ShowTransmittanceLut("Graphics/Atmosphere/Debug/Show Transmittance LUT", false);
    BoolVar s_EnableAtmosphere("Graphics/Atmosphere/Enable Atmosphere", true);
    NumVar s_CameraAltitudeKm("Graphics/Atmosphere/Camera Altitude Offset (km)", 0.0f, 0.0f, 300.0f, 5.0f);
    NumVar s_SunDiskIntensity("Graphics/Atmosphere/Sun Disk Intensity", 20.0f, 0.0f, 200.0f, 5.0f);

    BoolVar s_EnableMultiScattering("Graphics/Atmosphere/Enable Multi Scattering", true);
    BoolVar s_ShowMultiScatteringLut("Graphics/Atmosphere/Debug/Show Multi Scattering LUT", false);
    NumVar s_MultiScatteringDebugExposure("Graphics/Atmosphere/Debug/Multi Scattering Exposure", 20.0f, 1.0f, 200.0f, 5.0f);

    EnumVar s_SkyMode("Graphics/Atmosphere/Sky Mode", kSkyModeSkyViewLut, _countof(kSkyModeLabels), kSkyModeLabels);
    BoolVar s_ShowSkyViewLut("Graphics/Atmosphere/Debug/Show Sky View LUT", false);
    NumVar s_SkyViewDebugExposure("Graphics/Atmosphere/Debug/Sky View Exposure", 2.0f, 0.1f, 50.0f, 0.5f);

#pragma region AerialPerspective
    BoolVar s_EnableAerialPerspective("Graphics/Atmosphere/Enable Aerial Perspective", true);
    NumVar s_AerialPerspectiveDistanceScale("Graphics/Atmosphere/Debug/Aerial Perspective Distance Scale", 1.0f, 1.0f, 2000.0f, 50.0f);
    BoolVar s_ShowAerialPerspective("Graphics/Atmosphere/Debug/Show Aerial Perspective", false);
    NumVar s_AerialPerspectiveDebugSlice("Graphics/Atmosphere/Debug/Aerial Perspective Slice", 0.5f, 0.0f, 1.0f, 0.05f);
    NumVar s_AerialPerspectiveDebugExposure("Graphics/Atmosphere/Debug/Aerial Perspective Exposure", 10.0f, 1.0f, 200.0f, 5.0f);
#pragma endregion

    void BindCloudShadow(GraphicsContext& Context)
    {
        CloudShadowConstants Constants = s_CloudShadowConstants;
        if (!s_EnableAerialPerspective)
            Constants.ShaftMaxDistanceKm = 0.0f;

        Context.SetDynamicConstantBufferView(kCloudShadowCBV, sizeof(Constants), &Constants);
        Context.SetDynamicDescriptor(kSRVs, 5, s_CloudShadowSRV);
    }


    SkyViewConstants BuildViewConstants(const Math::Camera& Camera, const Math::Vector3& SunDirection, float SunIlluminance)
    {
        Math::Vector3 CameraPos = Camera.GetPosition();
        float CameraHeightKm = (float)CameraPos.GetY() * kWorldUnitsToKm + s_CameraAltitudeKm;

        SkyViewConstants ViewConstants = {};
        //카메라에서 이동을 제거하고 해야 정밀도 보장됨. 카메라 Translate를 그대로 두고 역행렬 계산하게 되면 부동소수점 오차가 발생해서 제대로 된 렌더링이 불가능
        Math::Matrix4 ViewRotation(Camera.GetViewMatrix().Get3x3(), Math::Vector3(Math::kZero));
        ViewConstants.InvViewProj = Math::Invert(Camera.GetProjMatrix() * ViewRotation);
        ViewConstants.CameraPositionKm[0] = (float)CameraPos.GetX() * kWorldUnitsToKm;
        ViewConstants.CameraPositionKm[1] = s_Constants.BottomRadius + Math::Max(CameraHeightKm, 0.01f);
        ViewConstants.CameraPositionKm[2] = (float)CameraPos.GetZ() * kWorldUnitsToKm;
        ViewConstants.SunIlluminance = SunIlluminance;
        ViewConstants.SunDirection[0] = (float)SunDirection.GetX();
        ViewConstants.SunDirection[1] = (float)SunDirection.GetY();
        ViewConstants.SunDirection[2] = (float)SunDirection.GetZ();
        ViewConstants.SunDiskCosAngle = cosf(kSunDiskHalfAngleRadians);
        ViewConstants.SunDiskIntensity = s_SunDiskIntensity;
        ViewConstants.MultiScatteringFactor = s_EnableMultiScattering ? 1.0f : 0.0f;
        ViewConstants.AerialPerspectiveWorldToKm = kWorldUnitsToKm * s_AerialPerspectiveDistanceScale;
        return ViewConstants;
    }

    float GetLength(const float(&Vector)[3])
    {
        return sqrtf(Vector[0] * Vector[0] + Vector[1] * Vector[1] + Vector[2] * Vector[2]);
    }

    void SetFloat3(float(&Dest)[3], float X, float Y, float Z)
    {
        Dest[0] = X;
        Dest[1] = Y;
        Dest[2] = Z;
    }

    void SetDefaultConstants(void)
    {
        SetFloat3(s_Constants.RayleighScattering, 0.005802f, 0.013558f, 0.033100f);
        s_Constants.RayleighScaleHeight = 8.0f;

        SetFloat3(s_Constants.MieScattering, 0.003996f, 0.003996f, 0.003996f);
        SetFloat3(s_Constants.MieExtinction, 0.004440f, 0.004440f, 0.004440f);
        s_Constants.MieScaleHeight = 1.2f;
        s_Constants.MiePhaseG = 0.8f;

        SetFloat3(s_Constants.OzoneExtinction, 0.000650f, 0.001881f, 0.000085f);
        s_Constants.OzoneCenterHeight = 25.0f;
        s_Constants.OzoneHalfWidth = 15.0f;

        SetFloat3(s_Constants.GroundAlbedo, 0.3f, 0.3f, 0.3f);
        s_Constants.BottomRadius = 6360.0f;
        s_Constants.TopRadius = 6460.0f;
        s_Constants.Pad[0] = s_Constants.Pad[1] = 0.0f;
    }

    void InitializeDebugViewPSO(GraphicsPSO& PSO, const void* PixelShader, size_t PixelShaderSize)
    {
        PSO.SetRootSignature(s_RootSig);
        PSO.SetRasterizerState(RasterizerTwoSided);
        PSO.SetBlendState(BlendDisable);
        PSO.SetDepthStencilState(DepthStateDisabled);
        PSO.SetInputLayout(0, nullptr);
        PSO.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
        PSO.SetRenderTargetFormat(g_OverlayBuffer.GetFormat(), DXGI_FORMAT_UNKNOWN);
        PSO.SetVertexShader(g_pSkyFullscreenVS, sizeof(g_pSkyFullscreenVS));
        PSO.SetPixelShader(PixelShader, PixelShaderSize);
        PSO.Finalize();
    }

    void InitializeComputePSO(ComputePSO& PSO, const void* ComputeShader, size_t ComputeShaderSize)
    {
        PSO.SetRootSignature(s_RootSig);
        PSO.SetComputeShader(ComputeShader, ComputeShaderSize);
        PSO.Finalize();
    }

    void InitializeSkyPSO(GraphicsPSO& PSO, const void* PixelShader, size_t PixelShaderSize)
    {
        PSO.SetRootSignature(s_RootSig);
        PSO.SetRasterizerState(RasterizerTwoSided);
        PSO.SetBlendState(BlendDisable);
        PSO.SetDepthStencilState(DepthStateReadOnly);
        PSO.SetInputLayout(0, nullptr);
        PSO.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
        PSO.SetRenderTargetFormat(g_SceneColorBuffer.GetFormat(), g_SceneDepthBuffer.GetFormat());
        PSO.SetVertexShader(g_pSkyFullscreenVS, sizeof(g_pSkyFullscreenVS));
        PSO.SetPixelShader(PixelShader, PixelShaderSize);
        PSO.Finalize();
    }
}

void Atmosphere::Initialize(void)
{
    s_RootSig.Reset(kNumRootBindings, 1);
    s_RootSig.InitStaticSampler(0, SamplerLinearClampDesc);
    s_RootSig[kAtmosphereCBV].InitAsConstantBuffer(0);
    s_RootSig[kDebugConstants].InitAsConstants(1, 4);
    s_RootSig[kViewCBV].InitAsConstantBuffer(2);
    s_RootSig[kCloudShadowCBV].InitAsConstantBuffer(4);
    s_RootSig[kSRVs].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 8);
    s_RootSig[kUAVs].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0, 4);
    s_RootSig.Finalize(L"Atmosphere");

    InitializeComputePSO(s_TransmittanceLutPSO, g_pAtmosphereTransmittanceLutCS, sizeof(g_pAtmosphereTransmittanceLutCS));
    InitializeComputePSO(s_MultiScatteringLutPSO, g_pAtmosphereMultiScatteringLutCS, sizeof(g_pAtmosphereMultiScatteringLutCS));
    InitializeDebugViewPSO(s_DebugViewPSO, g_pSkyDebugViewPS, sizeof(g_pSkyDebugViewPS));
    InitializeDebugViewPSO(s_DebugViewVolumePSO, g_pSkyDebugViewVolumePS, sizeof(g_pSkyDebugViewVolumePS));
    InitializeSkyPSO(s_AtmosphereRaymarchingReferencePSO, g_pAtmosphereRaymarchingReferencePS, sizeof(g_pAtmosphereRaymarchingReferencePS));
    InitializeComputePSO(s_SkyViewLutPSO, g_pAtmosphereSkyViewLutCS, sizeof(g_pAtmosphereSkyViewLutCS));
    InitializeSkyPSO(s_SkyViewCompositePSO, g_pAtmosphereSkyViewPS, sizeof(g_pAtmosphereSkyViewPS));
    InitializeComputePSO(s_AerialPerspectivePSO, g_pAtmosphereAerialPerspectiveCS, sizeof(g_pAtmosphereAerialPerspectiveCS));

    s_ApplyAerialPerspectivePSO.SetRootSignature(s_RootSig);
    s_ApplyAerialPerspectivePSO.SetRasterizerState(RasterizerTwoSided);
    s_ApplyAerialPerspectivePSO.SetBlendState(BlendPreMultiplied);
    s_ApplyAerialPerspectivePSO.SetDepthStencilState(DepthStateDisabled);
    s_ApplyAerialPerspectivePSO.SetInputLayout(0, nullptr);
    s_ApplyAerialPerspectivePSO.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
    s_ApplyAerialPerspectivePSO.SetRenderTargetFormat(g_SceneColorBuffer.GetFormat(), DXGI_FORMAT_UNKNOWN);
    s_ApplyAerialPerspectivePSO.SetVertexShader(g_pSkyFullscreenVS, sizeof(g_pSkyFullscreenVS));
    s_ApplyAerialPerspectivePSO.SetPixelShader(g_pAtmosphereAerialPerspectivePS, sizeof(g_pAtmosphereAerialPerspectivePS));
    s_ApplyAerialPerspectivePSO.Finalize();

    s_TransmittanceLut.Create(L"Atmosphere Transmittance LUT",
        kTransmittanceLutWidth, kTransmittanceLutHeight, 1, DXGI_FORMAT_R16G16B16A16_FLOAT);
    s_MultiScatteringLut.Create(L"Atmosphere Multi Scattering LUT",
        kMultiScatteringLutSize, kMultiScatteringLutSize, 1, DXGI_FORMAT_R16G16B16A16_FLOAT);
    s_SkyViewLut.Create(L"Atmosphere Sky View LUT",
        kSkyViewLutWidth, kSkyViewLutHeight, 1, DXGI_FORMAT_R16G16B16A16_FLOAT);
    s_AerialPerspectiveVolume.Create(L"Atmosphere Aerial Perspective",
        kAerialPerspectiveSize, kAerialPerspectiveSize, kAerialPerspectiveSize, 1, DXGI_FORMAT_R16G16B16A16_FLOAT);
    
    SetDefaultConstants();
    s_bTransmittanceDirty = true;
    s_bVolumeTestDirty = true;

    s_CloudShadowSRV = GetDefaultTexture(kBlackTransparent2D);
}

void Atmosphere::Shutdown(void)
{
    s_TransmittanceLut.Destroy();
    s_MultiScatteringLut.Destroy();
    s_SkyViewLut.Destroy();
    s_AerialPerspectiveVolume.Destroy();
}

void Atmosphere::Update(ComputeContext& Context)
{
    const bool bUpdateTransmittance = s_bTransmittanceDirty || s_ForceUpdateLuts;
    if (!bUpdateTransmittance && !s_bVolumeTestDirty)
        return;

    ScopedTimer _prof(L"Atmosphere LUTs", Context);

    Context.SetRootSignature(s_RootSig);
    Context.SetDynamicConstantBufferView(kAtmosphereCBV, sizeof(s_Constants), &s_Constants);

    if (bUpdateTransmittance)
    {
        {
            ScopedTimer _prof2(L"Transmittance LUT", Context);

            Context.TransitionResource(s_TransmittanceLut, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
            Context.SetPipelineState(s_TransmittanceLutPSO);
            Context.SetDynamicDescriptor(kUAVs, 0, s_TransmittanceLut.GetUAV());
            Context.Dispatch2D(kTransmittanceLutWidth, kTransmittanceLutHeight, 8, 8);
            Context.TransitionResource(s_TransmittanceLut, kShaderResourceState);
        }

        {
            ScopedTimer _prof3(L"Multi Scattering LUT", Context);

            Context.TransitionResource(s_MultiScatteringLut, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
            Context.SetPipelineState(s_MultiScatteringLutPSO);
            Context.SetDynamicDescriptor(kSRVs, 0, s_TransmittanceLut.GetSRV());
            Context.SetDynamicDescriptor(kUAVs, 0, s_MultiScatteringLut.GetUAV());
            Context.Dispatch3D(kMultiScatteringLutSize, kMultiScatteringLutSize, kMultiScatteringDirectionCount,
                1, 1, kMultiScatteringDirectionCount);
            Context.TransitionResource(s_MultiScatteringLut, kShaderResourceState);
        }

        s_bTransmittanceDirty = false;
    }

    if (s_bVolumeTestDirty)
    {
        s_bVolumeTestDirty = false;
    }
}

void Atmosphere::RenderSky(
    GraphicsContext& Context, const Math::Camera& Camera, const Math::Vector3& SunDirection,
    float SunIlluminance, const D3D12_VIEWPORT& Viewport, const D3D12_RECT& Scissor)
{
    ScopedTimer _prof(L"Atmosphere Sky", Context);

    SkyViewConstants ViewConstants = BuildViewConstants(Camera, SunDirection, SunIlluminance);
    s_LastViewConstants = ViewConstants;

    const bool bCameraInAtmosphere = GetLength(ViewConstants.CameraPositionKm) < s_Constants.TopRadius;
    const bool bUseSkyViewLut = (s_SkyMode == kSkyModeSkyViewLut) && bCameraInAtmosphere;

    if (bUseSkyViewLut)
    {
        ScopedTimer _prof2(L"Sky View LUT", Context);

        ComputeContext& Compute = Context.GetComputeContext();
        Compute.SetRootSignature(s_RootSig);
        Compute.SetPipelineState(s_SkyViewLutPSO);

        Compute.TransitionResource(s_TransmittanceLut, kShaderResourceState);
        Compute.TransitionResource(s_MultiScatteringLut, kShaderResourceState);
        Compute.TransitionResource(s_SkyViewLut, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);

        Compute.SetDynamicConstantBufferView(kAtmosphereCBV, sizeof(s_Constants), &s_Constants);
        Compute.SetDynamicConstantBufferView(kViewCBV, sizeof(ViewConstants), &ViewConstants);
        Compute.SetDynamicDescriptor(kSRVs, 0, s_TransmittanceLut.GetSRV());
        Compute.SetDynamicDescriptor(kSRVs, 1, s_MultiScatteringLut.GetSRV());
        Compute.SetDynamicDescriptor(kUAVs, 0, s_SkyViewLut.GetUAV());
        Compute.Dispatch2D(kSkyViewLutWidth, kSkyViewLutHeight, 8, 8);

        Compute.TransitionResource(s_SkyViewLut, kShaderResourceState);
    }

    if (s_EnableAerialPerspective)
    {
        ScopedTimer _prof4(L"Aerial Perspective", Context);

        {
            ComputeContext& Compute = Context.GetComputeContext();
            Compute.SetRootSignature(s_RootSig);
            Compute.SetPipelineState(s_AerialPerspectivePSO);

            Compute.TransitionResource(s_TransmittanceLut, kShaderResourceState);
            Compute.TransitionResource(s_MultiScatteringLut, kShaderResourceState);
            Compute.TransitionResource(s_AerialPerspectiveVolume, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);

            Compute.SetDynamicConstantBufferView(kAtmosphereCBV, sizeof(s_Constants), &s_Constants);
            Compute.SetDynamicConstantBufferView(kViewCBV, sizeof(ViewConstants), &ViewConstants);
            Compute.SetDynamicDescriptor(kSRVs, 0, s_TransmittanceLut.GetSRV());
            Compute.SetDynamicDescriptor(kSRVs, 1, s_MultiScatteringLut.GetSRV());
            Compute.SetDynamicDescriptor(kUAVs, 0, s_AerialPerspectiveVolume.GetUAV());
            Compute.Dispatch3D(kAerialPerspectiveSize, kAerialPerspectiveSize, kAerialPerspectiveSize, 8, 8, 1);

            Compute.TransitionResource(s_AerialPerspectiveVolume, kShaderResourceState);
        }

        Context.SetRootSignature(s_RootSig);
        Context.SetPipelineState(s_ApplyAerialPerspectivePSO);
        Context.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        Context.TransitionResource(s_AerialPerspectiveVolume, kShaderResourceState);
        Context.TransitionResource(g_SceneDepthBuffer, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Context.TransitionResource(g_SceneColorBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, true);
        Context.SetRenderTarget(g_SceneColorBuffer.GetRTV());
        Context.SetViewportAndScissor(Viewport, Scissor);

        Context.SetDynamicConstantBufferView(kAtmosphereCBV, sizeof(s_Constants), &s_Constants);
        Context.SetDynamicConstantBufferView(kViewCBV, sizeof(ViewConstants), &ViewConstants);
        Context.SetDynamicDescriptor(kSRVs, 3, s_AerialPerspectiveVolume.GetSRV());
        Context.SetDynamicDescriptor(kSRVs, 4, g_SceneDepthBuffer.GetDepthSRV());
        BindCloudShadow(Context);
        Context.Draw(3);
    }


    {
        ScopedTimer _prof3(L"Sky Composite", Context);

        Context.SetRootSignature(s_RootSig);
        Context.SetPipelineState(bUseSkyViewLut ? s_SkyViewCompositePSO : s_AtmosphereRaymarchingReferencePSO);
        Context.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        Context.TransitionResource(s_TransmittanceLut, kShaderResourceState);
        Context.TransitionResource(s_MultiScatteringLut, kShaderResourceState);
        Context.TransitionResource(s_SkyViewLut, kShaderResourceState);
        Context.TransitionResource(g_SceneDepthBuffer, D3D12_RESOURCE_STATE_DEPTH_READ);
        Context.TransitionResource(g_SceneColorBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, true);
        Context.SetRenderTarget(g_SceneColorBuffer.GetRTV(), g_SceneDepthBuffer.GetDSV_DepthReadOnly());
        Context.SetViewportAndScissor(Viewport, Scissor);

        Context.SetDynamicConstantBufferView(kAtmosphereCBV, sizeof(s_Constants), &s_Constants);
        Context.SetDynamicConstantBufferView(kViewCBV, sizeof(ViewConstants), &ViewConstants);
        Context.SetDynamicDescriptor(kSRVs, 0, s_TransmittanceLut.GetSRV());
        Context.SetDynamicDescriptor(kSRVs, 1, s_MultiScatteringLut.GetSRV());
        Context.SetDynamicDescriptor(kSRVs, 2, s_SkyViewLut.GetSRV());
        Context.SetDynamicDescriptor(kSRVs, 3, s_AerialPerspectiveVolume.GetSRV());
        BindCloudShadow(Context);
        Context.Draw(3);
    }


}

bool Atmosphere::IsAtmosphereEnabled(void)
{
    return s_EnableAtmosphere;
}

Math::Vector3 Atmosphere::ComputeSunTransmittance(float AltitudeKm, const Math::Vector3& SunDirection)
{

    const AtmosphereConstants& C = s_Constants;
    const float r = Math::Min(C.BottomRadius + Math::Max(AltitudeKm, 0.0f), C.TopRadius);

    const float Mu = (float)SunDirection.GetY();//천정각

    
    const float BottomRatio = C.BottomRadius / r;
    const float HorizonMu = -sqrtf(Math::Max(0.0f, 1.0f - BottomRatio * BottomRatio));
    if (Mu < HorizonMu)//행성 그림자
        return Math::Vector3(Math::kZero);

    const float B = r * Mu;
    const float RayLength = -B + sqrtf(Math::Max(0.0f, B * B - (r * r - C.TopRadius * C.TopRadius)));

    // AtmosphereTransmittanceLutCS
    const int kSteps = 40;
    const float StepSize = RayLength / kSteps;
    const float SinTheta = sqrtf(Math::Max(0.0f, 1.0f - Mu * Mu));
    float OpticalDepth[3] = { 0.0f, 0.0f, 0.0f };

    for (int i = 0; i < kSteps; ++i)
    {
        const float t = (i + 0.5f) * StepSize;
        const float X = t * SinTheta;
        const float Y = r + t * Mu;
        const float Height = sqrtf(X * X + Y * Y) - C.BottomRadius;

        const float RayleighDensity = expf(-Height / C.RayleighScaleHeight);
        const float MieDensity = expf(-Height / C.MieScaleHeight);
        const float OzoneDensity = Math::Max(0.0f, 1.0f - fabsf(Height - C.OzoneCenterHeight) / C.OzoneHalfWidth);

        for (int Channel = 0; Channel < 3; ++Channel)
        {
            OpticalDepth[Channel] += (C.RayleighScattering[Channel] * RayleighDensity
                + C.MieExtinction[Channel] * MieDensity
                + C.OzoneExtinction[Channel] * OzoneDensity) * StepSize;
        }
    }

    return Math::Vector3(expf(-OpticalDepth[0]), expf(-OpticalDepth[1]), expf(-OpticalDepth[2]));
}

void Atmosphere::BindSkyInputs(GraphicsContext& Context, UINT AtmosphereCBV, UINT ViewCBV, UINT SRVTable)
{
    Context.TransitionResource(s_TransmittanceLut, kShaderResourceState);
    Context.TransitionResource(s_SkyViewLut, kShaderResourceState);
    Context.TransitionResource(s_AerialPerspectiveVolume, kShaderResourceState);

    Context.SetDynamicConstantBufferView(AtmosphereCBV, sizeof(s_Constants), &s_Constants);
    Context.SetDynamicConstantBufferView(ViewCBV, sizeof(s_LastViewConstants), &s_LastViewConstants);
    Context.SetDynamicDescriptor(SRVTable, 0, s_TransmittanceLut.GetSRV());
    Context.SetDynamicDescriptor(SRVTable, 1, s_SkyViewLut.GetSRV());
    Context.SetDynamicDescriptor(SRVTable, 2, s_AerialPerspectiveVolume.GetSRV());
}

void Atmosphere::SetCloudShadow(D3D12_CPU_DESCRIPTOR_HANDLE ShadowMapSRV, const CloudShadowConstants& Constants)
{
    s_CloudShadowSRV = ShadowMapSRV;
    s_CloudShadowConstants = Constants;
}

void Atmosphere::RenderDebug(GraphicsContext& Context)
{
    if (!s_ShowTransmittanceLut && !s_ShowMultiScatteringLut && !s_ShowSkyViewLut && !s_ShowAerialPerspective)
        return;

    Context.SetRootSignature(s_RootSig);
    Context.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    Context.SetRenderTarget(g_OverlayBuffer.GetRTV());

    uint32_t Top = 0;

    if (s_ShowTransmittanceLut)
    {
        Context.TransitionResource(s_TransmittanceLut, kShaderResourceState, true);
        Context.SetPipelineState(s_DebugViewPSO);
        Context.SetDynamicDescriptor(kSRVs, 0, s_TransmittanceLut.GetSRV());
        Context.SetConstants(kDebugConstants, 1.0f, 0.0f);
        Context.SetViewportAndScissor(kDebugViewMargin, Top,
            kTransmittanceLutWidth * kDebugViewScale, kTransmittanceLutHeight * kDebugViewScale);
        Context.Draw(3);

        Top += kTransmittanceLutHeight * kDebugViewScale + kDebugViewMargin;
    }

    if (s_ShowMultiScatteringLut)
    {
        Context.TransitionResource(s_MultiScatteringLut, kShaderResourceState, true);
        Context.SetPipelineState(s_DebugViewPSO);
        Context.SetDynamicDescriptor(kSRVs, 0, s_MultiScatteringLut.GetSRV());
        Context.SetConstants(kDebugConstants, (float)s_MultiScatteringDebugExposure, 0.0f);
        Context.SetViewportAndScissor(kDebugViewMargin, Top,
            kMultiScatteringLutSize * kDebugViewScale * 2, kMultiScatteringLutSize * kDebugViewScale * 2);
        Context.Draw(3);

        Top += kMultiScatteringLutSize * kDebugViewScale * 2 + kDebugViewMargin;
    }
    if (s_ShowSkyViewLut)
    {
        Context.TransitionResource(s_SkyViewLut, kShaderResourceState, true);
        Context.SetPipelineState(s_DebugViewPSO);
        Context.SetDynamicDescriptor(kSRVs, 0, s_SkyViewLut.GetSRV());
        Context.SetConstants(kDebugConstants, (float)s_SkyViewDebugExposure, 0.0f);
        Context.SetViewportAndScissor(kDebugViewMargin, Top, kSkyViewLutWidth * 2, kSkyViewLutHeight * 2);
        Context.Draw(3);

        Top += kSkyViewLutHeight * 2 + kDebugViewMargin;
    }

    if (s_ShowAerialPerspective)
    {
        Context.TransitionResource(s_AerialPerspectiveVolume, kShaderResourceState, true);
        Context.SetPipelineState(s_DebugViewVolumePSO);
        Context.SetDynamicDescriptor(kSRVs, 0, s_AerialPerspectiveVolume.GetSRV());
        Context.SetConstants(kDebugConstants, (float)s_AerialPerspectiveDebugExposure, (float)s_AerialPerspectiveDebugSlice);
        Context.SetViewportAndScissor(kDebugViewMargin, Top, kAerialPerspectiveSize * 8, kAerialPerspectiveSize * 8);
        Context.Draw(3);

        Top += kAerialPerspectiveSize * 8 + kDebugViewMargin;
    }

}