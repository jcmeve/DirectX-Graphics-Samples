#pragma once

class GraphicsContext;
class ComputeContext;

namespace Atmosphere
{
	void Initialize();
	void Shutdown();

	void Update(ComputeContext& Context);
	void RenderDebug(GraphicsContext& Context);
};

