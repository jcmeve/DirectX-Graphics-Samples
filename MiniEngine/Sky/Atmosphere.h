#pragma once

#include <d3d12.h>

class GraphicsContext;
class ComputeContext;

namespace Math
{
	class Camera;
	class Vector3;
}

namespace Atmosphere
{
	void Initialize();
	void Shutdown();

	void Update(ComputeContext& Context);
		
	void RenderSky(GraphicsContext& Context, const Math::Camera& Camera, const Math::Vector3& SunDirection,
		float SunIlluminance, const D3D12_VIEWPORT& Viewport, const D3D12_RECT& Scissor);

	bool IsAtmosphereEnabled(void);

	Math::Vector3 ComputeSunTransmittance(float AltitudeKm, const Math::Vector3& SunDirection);

	//외부에서 필요 시 바인딩
	void BindSkyInputs(GraphicsContext& Context, UINT AtmosphereCBV, UINT ViewCBV, UINT SRVTable);

	void RenderDebug(GraphicsContext& Context);
};

