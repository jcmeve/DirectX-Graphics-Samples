#include "Clouds.h"
#include "RootSignature.h"
#include "PipelineState.h"
#include "CommandContext.h"
#include "GraphicsCommon.h"
#include "BufferManager.h"
#include "TextureManager.h"
#include "EngineTuning.h"

#include "CompiledShaders/SkyFullscreenVS.h"
#include "CompiledShaders/SkyDebugViewVolumePS.h"

using namespace Graphics;

namespace
{
    enum RootBindings
    {
        kCloudCBV,          //b0
        kDebugConstants,    //b1
        kViewCBV,           //b2
        kSRVs,              //t0 ~ t7
        kUAVs,              //u0 ~ u3
        kNumRootBindings
    };

    const uint32_t kDebugViewSize = 384;
    const uint32_t kDebugViewMargin = 16;

    const wchar_t* kNoisePath = L"NubisVoxelCloudsPack/DDS/NubisNoise.dds";
    const wchar_t* kModelingPath = L"NubisVoxelCloudsPack/DDS/ParkouringCloud_Modeling.dds";
    const wchar_t* kFieldPath = L"NubisVoxelCloudsPack/DDS/ParkouringCloud_Field.dds";

    RootSignature s_RootSig;
    GraphicsPSO s_DebugViewVolumePSO(L"Clouds: Debug View Volume PSO");

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
}

void Clouds::Initialize(void)
{
    // s0 = 디버그 뷰용 Clamp, s1 = 타일링 노이즈 조회용 Wrap
    s_RootSig.Reset(kNumRootBindings, 2);
    s_RootSig.InitStaticSampler(0, SamplerLinearClampDesc);
    s_RootSig.InitStaticSampler(1, SamplerLinearWrapDesc);
    s_RootSig[kCloudCBV].InitAsConstantBuffer(0);
    s_RootSig[kDebugConstants].InitAsConstants(1, 4);
    s_RootSig[kViewCBV].InitAsConstantBuffer(2);
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