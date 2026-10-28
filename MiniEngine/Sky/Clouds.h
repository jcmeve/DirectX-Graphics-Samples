#pragma once

class GraphicsContext;

namespace Clouds
{
	void Initialize(void);
	void Shutdown(void);

	void RenderDebug(GraphicsContext& Context);
};
