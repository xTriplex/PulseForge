#pragma once

#include "Renderer/SceneRenderer.h"

namespace PulseForge
{
	class SceneRenderer::ShadowTargetCache final
	{
	public:
		explicit ShadowTargetCache(RenderDevice& Runtime) noexcept;

		[[nodiscard]] std::expected<void, SceneRendererError> Initialize(const BindingLayoutHandle& DirectionalLayout,
			const BindingLayoutHandle& SpotlightLayout, const BindingLayoutHandle& PointLightLayout, const Sampler& ShadowSampler,
			const Texture& FallbackPointShadowTexture);
		[[nodiscard]] std::expected<void, SceneRendererError> EnsureSpotlightMaps(
			const BindingLayoutHandle& Layout, const Sampler& ShadowSampler);
		[[nodiscard]] std::expected<void, SceneRendererError> EnsurePointLightCubemaps(
			const BindingLayoutHandle& Layout, const Sampler& ShadowSampler);

		[[nodiscard]] const RenderTarget& DirectionalTarget(size_t CascadeIndex) const noexcept;
		[[nodiscard]] const Texture* DirectionalShadowTexture(size_t CascadeIndex) const noexcept;
		[[nodiscard]] const BindingSet& DirectionalBindings() const noexcept;
		[[nodiscard]] const BindingSet& SpotlightBindings() const noexcept;
		[[nodiscard]] const BindingSet& PointLightBindings() const noexcept;
		[[nodiscard]] const RenderTarget* SpotlightTarget(size_t Slot) const noexcept;
		[[nodiscard]] const Texture* SpotlightShadowTexture(size_t Slot) const noexcept;
		[[nodiscard]] const DepthCubemap* PointLightCubemap(size_t Slot) const noexcept;
		[[nodiscard]] const Texture* PointShadowFaceTexture(size_t Slot, uint32_t Face) const noexcept;
		[[nodiscard]] bool HasSpotlightMaps() const noexcept;
		[[nodiscard]] bool HasPointLightCubemaps() const noexcept;
		[[nodiscard]] uint64_t SpotlightResourceGeneration() const noexcept;
		[[nodiscard]] uint64_t PointLightResourceGeneration() const noexcept;

	private:
		RenderDevice& m_Runtime;
		std::array<RenderTargetHandle, 4> m_DirectionalTargets;
		std::array<RenderTargetHandle, MaxSpotlightShadowMapCount> m_SpotlightTargets;
		std::array<DepthCubemapHandle, MaxPointLightShadowCount> m_PointLightCubemaps;
		BindingSetHandle m_DirectionalBindings;
		BindingSetHandle m_SpotlightBindings;
		BindingSetHandle m_PointLightBindings;
		uint64_t m_SpotlightResourceGeneration = 0;
		uint64_t m_PointLightResourceGeneration = 0;
	};
} // namespace PulseForge
