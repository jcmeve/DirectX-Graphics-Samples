#pragma once

#include <d3d12.h>
#include "CloudShadowConstants.h"

class GraphicsContext;
class ComputeContext;

namespace Math
{
	class Camera;
	class Vector3;
}

namespace Clouds
{
	void Initialize(void);
	void Shutdown(void);

	void Render(GraphicsContext& Context, const Math::Camera& Camera, const D3D12_VIEWPORT& Viewport, const D3D12_RECT& Scissor);

	void RenderDebug(GraphicsContext& Context);

	void RenderShadowMap(ComputeContext& Context, const Math::Camera& Camera, const Math::Vector3& SunDirection);
	D3D12_CPU_DESCRIPTOR_HANDLE GetShadowMapSRV(void);
	const CloudShadowConstants& GetShadowConstants(void);
};