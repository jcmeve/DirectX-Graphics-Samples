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

#include "CompiledShaders/CloudRaymarchingPS.h"
#include "CompiledShaders/SkyFullscreenVS.h"
#include "CompiledShaders/SkyDebugViewVolumePS.h"
#include <Display.h>

using namespace Graphics;

namespace
{
    enum RootBindings
    {
        kAtmosphereCBV,     //b0
        kDebugConstants,    //b1
        kViewCBV,           //b2
        kCloudCBV,          //b3
        kSRVs,              //t0 ~ t7
        kUAVs,              //u0 ~ u3
        kNumRootBindings
    };

    const uint32_t kDebugViewSize = 384;
    const uint32_t kDebugViewMargin = 16;

    const wchar_t* kNoisePath = L"NubisVoxelCloudsPack/DDS/NubisNoise.dds";
    const wchar_t* kModelingPath = L"NubisVoxelCloudsPack/DDS/ParkouringCloud_Modeling.dds";
    const wchar_t* kFieldPath = L"NubisVoxelCloudsPack/DDS/ParkouringCloud_Field.dds";

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
    };

    const float kWorldUnitsToMeters = 0.01f; //cm to m
    const float kCloudBoundsSize[3] = { 4096.0f, 512.0f, 4096.0f }; 

    RootSignature s_RootSig;
    GraphicsPSO s_DebugViewVolumePSO(L"Clouds: Debug View Volume PSO");
    GraphicsPSO s_RaymarchPSO(L"Clouds: Raymarch PSO");

    BoolVar s_EnableClouds("Graphics/Clouds/Enable", true);
    NumVar s_CloudBaseAltitude("Graphics/Clouds/Base Altitude (m)", 2000.0f, 0.0f, 8000.0f, 100.0f);
    NumVar s_CloudOffsetX("Graphics/Clouds/Offset X (m)", 0.0f, -20000.0f, 20000.0f, 100.0f);
    NumVar s_CloudOffsetZ("Graphics/Clouds/Offset Z (m)", 0.0f, -20000.0f, 20000.0f, 100.0f);
    NumVar s_CloudExtinction("Graphics/Clouds/Extinction (1 per m)", 0.05f, 0.001f, 0.5f, 0.005f);
    NumVar s_CloudStepScale("Graphics/Clouds/Step Scale", 0.08f, 0.01f, 1.0f, 0.01f);
    IntVar s_CloudMaxSteps("Graphics/Clouds/Max Steps", 256, 16, 1024, 16);
    NumVar s_CloudAmbientScale("Graphics/Clouds/Ambient Scale", 1.0f, 0.0f, 4.0f, 0.1f);
    NumVar s_CloudMipDistanceScale("Graphics/Clouds/Noise Mip Distance Scale", 0.004f, 0.0f, 0.05f, 0.001f);

    TextureRef s_NoiseTexture; //128*128*128 노이즈 RGBA 각각
    TextureRef s_ModelingTexture; //R: Dimensional Profile, G: Detail Type, B: Density Scale 512*512*64  
    TextureRef s_FieldTexture; //R채널 512*512*64 SDF
    bool s_bTexturesValid = false;

    enum DebugVolume
    {
        kDebugVolumeNone,
        kDebugVolumeNoise,
        kDebugVolumeModeling,
        kDebugVolumeField,
    };
    const char* kDebugVolumeLabels[] = { "None", "Noise", "Modeling", "Field (SDF)" };

    EnumVar s_DebugVolume("Graphics/Clouds/Debug/Show Volume", kDebugVolumeNone, _countof(kDebugVolumeLabels), kDebugVolumeLabels);
    NumVar s_DebugVolumeSlice("Graphics/Clouds/Debug/Volume Slice", 0.45f, 0.0f, 1.0f, 0.01f);

    const TextureRef& GetDebugTexture(void)
    {
        switch (s_DebugVolume)
        {
        case kDebugVolumeNoise:    return s_NoiseTexture;
        case kDebugVolumeModeling: return s_ModelingTexture;
        default:                   return s_FieldTexture;
        }
    }

    CloudConstants BuildCloudConstants(const Math::Camera& Camera)
    {
        Math::Vector3 CameraPos = Camera.GetPosition() * kWorldUnitsToMeters;
        const float Camera3[3] = { (float)CameraPos.GetX(), (float)CameraPos.GetY(), (float)CameraPos.GetZ() };
        const float BoundsMin[3] = { s_CloudOffsetX - kCloudBoundsSize[0] * 0.5f, (float)s_CloudBaseAltitude, s_CloudOffsetZ - kCloudBoundsSize[2] * 0.5f };

        CloudConstants Constants = {};
        for (int Axis = 0; Axis < 3; ++Axis)
        {
            Constants.BoundsMin[Axis] = BoundsMin[Axis] - Camera3[Axis];
            Constants.BoundsSize[Axis] = kCloudBoundsSize[Axis];
            Constants.CameraPosition[Axis] = Camera3[Axis];
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
    s_RootSig[kSRVs].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 8);
    s_RootSig[kUAVs].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0, 4);
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

    s_RaymarchPSO.SetRootSignature(s_RootSig);
    s_RaymarchPSO.SetRasterizerState(RasterizerTwoSided);
    s_RaymarchPSO.SetBlendState(BlendPreMultiplied);
    s_RaymarchPSO.SetDepthStencilState(DepthStateDisabled);
    s_RaymarchPSO.SetInputLayout(0, nullptr);
    s_RaymarchPSO.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
    s_RaymarchPSO.SetRenderTargetFormat(g_SceneColorBuffer.GetFormat(), DXGI_FORMAT_UNKNOWN);
    s_RaymarchPSO.SetVertexShader(g_pSkyFullscreenVS, sizeof(g_pSkyFullscreenVS));
    s_RaymarchPSO.SetPixelShader(g_pCloudRaymarchingPS, sizeof(g_pCloudRaymarchingPS));
    s_RaymarchPSO.Finalize();

    s_NoiseTexture = TextureManager::LoadDDSFromFile(kNoisePath);
    s_ModelingTexture = TextureManager::LoadDDSFromFile(kModelingPath);
    s_FieldTexture = TextureManager::LoadDDSFromFile(kFieldPath);

    s_bTexturesValid = s_NoiseTexture.IsValid() && s_ModelingTexture.IsValid() && s_FieldTexture.IsValid();
    ASSERT(s_bTexturesValid);
}

void Clouds::Shutdown(void)
{
    s_NoiseTexture = nullptr;
    s_ModelingTexture = nullptr;
    s_FieldTexture = nullptr;
}

void Clouds::Render(GraphicsContext& Context, const Math::Camera& Camera, const D3D12_VIEWPORT& Viewport, const D3D12_RECT& Scissor)
{
    if (!s_EnableClouds || !s_bTexturesValid)
        return;

    ScopedTimer _prof(L"Clouds", Context);

    CloudConstants Constants = BuildCloudConstants(Camera);

    Context.SetRootSignature(s_RootSig);
    Context.SetPipelineState(s_RaymarchPSO);
    Context.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    Context.TransitionResource(g_SceneDepthBuffer, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Context.TransitionResource(g_SceneColorBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, true);
    Context.SetRenderTarget(g_SceneColorBuffer.GetRTV());
    Context.SetViewportAndScissor(Viewport, Scissor);

    // t0~t2 Atmosphere에 바인딩 위임
    Atmosphere::BindSkyInputs(Context, kAtmosphereCBV, kViewCBV, kSRVs);
    Context.SetDynamicConstantBufferView(kCloudCBV, sizeof(Constants), &Constants);
    Context.SetDynamicDescriptor(kSRVs, 3, g_SceneDepthBuffer.GetDepthSRV());
    Context.SetDynamicDescriptor(kSRVs, 4, s_NoiseTexture.GetSRV());
    Context.SetDynamicDescriptor(kSRVs, 5, s_ModelingTexture.GetSRV());
    Context.SetDynamicDescriptor(kSRVs, 6, s_FieldTexture.GetSRV());
    Context.Draw(3);
}

void Clouds::RenderDebug(GraphicsContext& Context)
{
    if (s_DebugVolume == kDebugVolumeNone || !s_bTexturesValid)
        return;

    Context.SetRootSignature(s_RootSig);
    Context.SetPipelineState(s_DebugViewVolumePSO);
    Context.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    Context.SetRenderTarget(g_OverlayBuffer.GetRTV());
    Context.SetDynamicDescriptor(kSRVs, 0, GetDebugTexture().GetSRV());
    Context.SetConstants(kDebugConstants, 1.0f, (float)s_DebugVolumeSlice);

    Context.SetViewportAndScissor(g_OverlayBuffer.GetWidth() - kDebugViewSize - kDebugViewMargin, kDebugViewMargin,
        kDebugViewSize, kDebugViewSize);
    Context.Draw(3);
}