#pragma once

#include <d3d12.h>

class GraphicsContext;

namespace Math
{
	class Camera;
}

namespace Clouds
{
	void Initialize(void);
	void Shutdown(void);

	// Atmosphere::RenderSky 이후 호출. 씬 컬러에 프리멀티플라이드로 합성
	void Render(GraphicsContext& Context, const Math::Camera& Camera, const D3D12_VIEWPORT& Viewport, const D3D12_RECT& Scissor);

	void RenderDebug(GraphicsContext& Context);
};