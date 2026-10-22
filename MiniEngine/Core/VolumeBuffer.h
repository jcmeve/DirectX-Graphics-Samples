#pragma once
#include "PixelBuffer.h"

class VolumeBuffer : public PixelBuffer
{
public:
	VolumeBuffer() : m_NumMips(0) 
	{
		m_SRVHandle.ptr = D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN;
		for (int i = 0; i < _countof(m_UAVHandle); ++i) 
		{
			m_UAVHandle[i].ptr = D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN;
		}
	}

	void Create(const std::wstring& Name, uint32_t Width, uint32_t Height, uint32_t Depth, uint32_t NumMips, DXGI_FORMAT Format);

	const D3D12_CPU_DESCRIPTOR_HANDLE GetSRV(void) const { return m_SRVHandle; }
	const D3D12_CPU_DESCRIPTOR_HANDLE GetUAV(uint32_t MipLevel = 0) const { ASSERT(MipLevel >= 0 && MipLevel < m_NumMips); return m_UAVHandle[MipLevel]; }

	uint32_t GetNumMips(void) const { return m_NumMips; }

private:

	D3D12_CPU_DESCRIPTOR_HANDLE m_SRVHandle;
	D3D12_CPU_DESCRIPTOR_HANDLE m_UAVHandle[12];
	uint32_t m_NumMips;
};

