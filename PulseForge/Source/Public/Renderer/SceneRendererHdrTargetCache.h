#pragma once

#include "Renderer/SceneRenderer.h"

namespace PulseForge
{
	class SceneRenderer::HdrTargetCache final
	{
	public:
		struct EnsureResult final
		{
			const RenderTarget* Target = nullptr;
			uint64_t Generation = 0;
		};

		explicit HdrTargetCache(RenderDevice& Runtime) noexcept;

		[[nodiscard]] bool NeedsReplacement(uint32_t Width, uint32_t Height) const noexcept;
		[[nodiscard]] std::expected<EnsureResult, SceneRendererError> Ensure(uint32_t Width, uint32_t Height);
#if defined(PF_DEBUG)
		[[nodiscard]] const Texture* GetColorTextureForValidation() const noexcept;
		[[nodiscard]] const Texture* GetDepthTextureForValidation() const noexcept;
#endif

	private:
		[[nodiscard]] bool CanReuse(uint32_t Width, uint32_t Height) const noexcept;

		RenderDevice& m_Runtime;
		RenderTargetHandle m_Target;
		uint64_t m_Generation = 0;
	};
} // namespace PulseForge
