#include "pch.h"
#include "GraphicsCore.h"
#include "VolumeBuffer.h"

void VolumeBuffer::Create(const std::wstring& Name, uint32_t Width, uint32_t Height, uint32_t Depth, uint32_t NumMips, DXGI_FORMAT Format)
{
	ASSERT(NumMips >= 1 && NumMips < _countof(m_UAVHandle));
	Destroy();

	m_Width = Width;
	m_Height = Height;
	m_ArraySize = Depth;
	m_Format = Format;
	m_NumMips = NumMips;

    D3D12_RESOURCE_DESC ResourceDesc = CD3DX12_RESOURCE_DESC::Tex3D(GetBaseFormat(Format),
        Width, Height, (UINT16)Depth, (UINT16)NumMips, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    CD3DX12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    ASSERT_SUCCEEDED(Graphics::g_Device->CreateCommittedResource(&HeapProps, D3D12_HEAP_FLAG_NONE,
        &ResourceDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, MY_IID_PPV_ARGS(&m_pResource)));

    m_UsageState = D3D12_RESOURCE_STATE_COMMON;
    m_GpuVirtualAddress = D3D12_GPU_VIRTUAL_ADDRESS_NULL;

#ifndef RELEASE
    m_pResource->SetName(Name.c_str());
#endif

    ID3D12Resource* Resource = m_pResource.Get();

    D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc = {};
    SRVDesc.Format = Format;
    SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    SRVDesc.Texture3D.MostDetailedMip = 0;
    SRVDesc.Texture3D.MipLevels = m_NumMips;

    if (m_SRVHandle.ptr == D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN)
        m_SRVHandle = Graphics::AllocateDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    Graphics::g_Device->CreateShaderResourceView(Resource, &SRVDesc, m_SRVHandle);

    // ¹Ó ·¹º§¸¶´Ù UAV
    D3D12_UNORDERED_ACCESS_VIEW_DESC UAVDesc = {};
    UAVDesc.Format = GetUAVFormat(Format);
    UAVDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
    UAVDesc.Texture3D.FirstWSlice = 0;
    UAVDesc.Texture3D.WSize = (UINT)-1;

    for (uint32_t MipLevel = 0; MipLevel < m_NumMips; ++MipLevel)
    {
        if (m_UAVHandle[MipLevel].ptr == D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN)
            m_UAVHandle[MipLevel] = Graphics::AllocateDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        UAVDesc.Texture3D.MipSlice = MipLevel;
        Graphics::g_Device->CreateUnorderedAccessView(Resource, nullptr, &UAVDesc, m_UAVHandle[MipLevel]);
    }

}

