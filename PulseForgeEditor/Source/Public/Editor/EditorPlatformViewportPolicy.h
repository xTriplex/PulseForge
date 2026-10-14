#pragma once

#include <utility>

namespace PulseForgeEditor
{
	// Keep each viewport's render and present together. This matters for renderers whose immediate
	// command context can have only one command list open at a time.
	template<typename PlatformRender, typename RendererRender, typename PlatformSwap, typename RendererSwap>
	inline void RenderPlatformViewportSequentially(
		PlatformRender&& RenderPlatform,
		RendererRender&& RenderRenderer,
		PlatformSwap&& SwapPlatform,
		RendererSwap&& SwapRenderer)
	{
		std::forward<PlatformRender>(RenderPlatform)();
		std::forward<RendererRender>(RenderRenderer)();
		std::forward<PlatformSwap>(SwapPlatform)();
		std::forward<RendererSwap>(SwapRenderer)();
	}
}
