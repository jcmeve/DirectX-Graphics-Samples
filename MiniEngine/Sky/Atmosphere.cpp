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

#include "CompiledShaders/AtmosphereTransmittanceLutCS.h"
#include "CompiledShaders/AtmosphereVolumeTestCS.h"
#include "CompiledShaders/SkyFullscreenVS.h"
#include "CompiledShaders/SkyDebugViewPS.h"
#include "CompiledShaders/SkyDebugViewVolumePS.h"

using namespace Graphics;

namespace
{
    // AtmosphereCommon.hlsli의 cbuffer와 같은 배치 (Math::Vector3는 16바이트라 쓰지 않음)
    __declspec(align(16)) struct AtmosphereConstants
    {
        float RayleighScattering[3];    float RayleighScaleHeight;
        float MieScattering[3];         float MieScaleHeight;
        float MieAbsorption[3];         float MiePhaseG;
        float OzoneAbsorption[3];       float OzoneCenterHeight;
        float GroundAlbedo[3];          float OzoneHalfWidth;
        float BottomRadius;             float TopRadius;    float Pad[2];
    };
    static_assert(sizeof(AtmosphereConstants) == 96, "HLSL AtmosphereConstants와 크기가 다름");

    enum RootBindings
    {
        kAtmosphereCBV,     // b0
        kDebugConstants,    // b1
        kSRVs,              // t0 ~ t3
        kUAVs,              // u0 ~ u3

        kNumRootBindings
    };

    const uint32_t kTransmittanceLutWidth = 256;
    const uint32_t kTransmittanceLutHeight = 64;
    const uint32_t kVolumeTestSize = 32;
    const uint32_t kDebugViewScale = 3;
    const uint32_t kDebugViewMargin = 16;
    const D3D12_RESOURCE_STATES kShaderResourceState =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    RootSignature s_RootSig;
    ComputePSO s_TransmittanceLutPSO(L"Atmosphere: Transmittance LUT CS");
    ComputePSO s_VolumeTestPSO(L"Atmosphere: Volume Test CS");
    GraphicsPSO s_DebugViewPSO(L"Atmosphere: Debug View PSO");
    GraphicsPSO s_DebugViewVolumePSO(L"Atmosphere: Debug View Volume PSO");

    ColorBuffer s_TransmittanceLut;
    VolumeBuffer s_VolumeTest;

    AtmosphereConstants s_Constants;
    bool s_bTransmittanceDirty = true;
    bool s_bVolumeTestDirty = true;

    BoolVar s_ForceUpdateLuts("Graphics/Atmosphere/Force Update LUTs", false);
    BoolVar s_ShowTransmittanceLut("Graphics/Atmosphere/Debug/Show Transmittance LUT", false);
    BoolVar s_ShowVolumeTest("Graphics/Atmosphere/Debug/Show Volume Test", false);
    NumVar s_VolumeTestSlice("Graphics/Atmosphere/Debug/Volume Slice", 0.5f, 0.0f, 1.0f, 0.05f);

    void SetFloat3(float(&Dest)[3], float X, float Y, float Z)
    {
        Dest[0] = X;
        Dest[1] = Y;
        Dest[2] = Z;
    }

    // 지구 대기 기본값 (단위 km, /km). 노트 6절 – 참고 구현에서 확인 필요
    void SetDefaultConstants(void)
    {
        SetFloat3(s_Constants.RayleighScattering, 5.802e-3f, 13.558e-3f, 33.1e-3f);
        s_Constants.RayleighScaleHeight = 8.0f;

        SetFloat3(s_Constants.MieScattering, 3.996e-3f, 3.996e-3f, 3.996e-3f);
        SetFloat3(s_Constants.MieAbsorption, 4.40e-3f, 4.40e-3f, 4.40e-3f);
        s_Constants.MieScaleHeight = 1.2f;
        s_Constants.MiePhaseG = 0.8f;

        SetFloat3(s_Constants.OzoneAbsorption, 0.650e-3f, 1.881e-3f, 0.085e-3f);
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
}

void Atmosphere::Initialize(void)
{
    s_RootSig.Reset(kNumRootBindings, 1);
    s_RootSig.InitStaticSampler(0, SamplerLinearClampDesc);
    s_RootSig[kAtmosphereCBV].InitAsConstantBuffer(0);
    s_RootSig[kDebugConstants].InitAsConstants(1, 4);
    s_RootSig[kSRVs].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 4);
    s_RootSig[kUAVs].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0, 4);
    s_RootSig.Finalize(L"Atmosphere");

    InitializeComputePSO(s_TransmittanceLutPSO, g_pAtmosphereTransmittanceLutCS, sizeof(g_pAtmosphereTransmittanceLutCS));
    InitializeComputePSO(s_VolumeTestPSO, g_pAtmosphereVolumeTestCS, sizeof(g_pAtmosphereVolumeTestCS));
    InitializeDebugViewPSO(s_DebugViewPSO, g_pSkyDebugViewPS, sizeof(g_pSkyDebugViewPS));
    InitializeDebugViewPSO(s_DebugViewVolumePSO, g_pSkyDebugViewVolumePS, sizeof(g_pSkyDebugViewVolumePS));

    s_TransmittanceLut.Create(L"Atmosphere Transmittance LUT",
        kTransmittanceLutWidth, kTransmittanceLutHeight, 1, DXGI_FORMAT_R16G16B16A16_FLOAT);
    s_VolumeTest.Create(L"Atmosphere Volume Test",
        kVolumeTestSize, kVolumeTestSize, kVolumeTestSize, 1, DXGI_FORMAT_R8G8B8A8_UNORM);

    SetDefaultConstants();
    s_bTransmittanceDirty = true;
    s_bVolumeTestDirty = true;
}

void Atmosphere::Shutdown(void)
{
    s_TransmittanceLut.Destroy();
    s_VolumeTest.Destroy();
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
        ScopedTimer _prof2(L"Transmittance LUT", Context);

        Context.TransitionResource(s_TransmittanceLut, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
        Context.SetPipelineState(s_TransmittanceLutPSO);
        Context.SetDynamicDescriptor(kUAVs, 0, s_TransmittanceLut.GetUAV());
        Context.Dispatch2D(kTransmittanceLutWidth, kTransmittanceLutHeight, 8, 8);
        Context.TransitionResource(s_TransmittanceLut, kShaderResourceState);

        s_bTransmittanceDirty = false;
    }

    if (s_bVolumeTestDirty)
    {
        Context.TransitionResource(s_VolumeTest, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
        Context.SetPipelineState(s_VolumeTestPSO);
        Context.SetDynamicDescriptor(kUAVs, 0, s_VolumeTest.GetUAV());
        Context.Dispatch3D(kVolumeTestSize, kVolumeTestSize, kVolumeTestSize, 4, 4, 4);
        Context.TransitionResource(s_VolumeTest, kShaderResourceState);

        s_bVolumeTestDirty = false;
    }
}

void Atmosphere::RenderDebug(GraphicsContext& Context)
{
    if (!s_ShowTransmittanceLut && !s_ShowVolumeTest)
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

    if (s_ShowVolumeTest)
    {
        Context.TransitionResource(s_VolumeTest, kShaderResourceState, true);
        Context.SetPipelineState(s_DebugViewVolumePSO);
        Context.SetDynamicDescriptor(kSRVs, 0, s_VolumeTest.GetSRV());
        Context.SetConstants(kDebugConstants, 1.0f, (float)s_VolumeTestSlice);
        Context.SetViewportAndScissor(kDebugViewMargin, Top,
            kVolumeTestSize * kDebugViewScale * 2, kVolumeTestSize * kDebugViewScale * 2);
        Context.Draw(3);
    }
}