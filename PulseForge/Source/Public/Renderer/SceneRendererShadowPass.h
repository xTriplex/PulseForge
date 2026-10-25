#pragma once

#include "Renderer/SceneRenderer.h"

#include <utility>
#include <vector>

namespace PulseForge
{
	class MeshAssetCache;

	class SceneRenderer::ShadowPass final
	{
	public:
		struct Statistics final
		{
			size_t SpotlightPassCount = 0;
			size_t SpotlightCasterDrawCount = 0;
			size_t PointPassCount = 0;
			size_t PointCasterDrawCount = 0;
		};

		struct Resources final
		{
			RenderDevice& Runtime;
			const SceneRenderSnapshot* Snapshot;
			const DirectionalShadowCascadeSet* Cascades;
			LocalLightingState& LocalLighting;
			ShadowTargetCache& Targets;
			SceneRendererPipelineCache& Pipelines;
			SceneRendererMaterialBindingCache& MaterialBindings;
			const std::vector<std::pair<AssetID, const Mesh*>>& PreparedMeshes;
			const std::vector<std::pair<UUID, const BindingSet*>>& PreparedMaterialBindings;
			const BindingLayoutHandle& SpotlightLayout;
			const BindingLayoutHandle& PointLightLayout;
			const Sampler& ShadowSampler;
			const BufferHandle& ShadowObjectConstants;
			const BindingSetHandle& ShadowObjectBindings;
			const BufferHandle& PointShadowObjectConstants;
			const BindingSetHandle& PointShadowObjectBindings;
		};

		[[nodiscard]] std::expected<void, SceneRendererError> Render(const Resources& Resources);
		[[nodiscard]] const Statistics& GetStatistics() const noexcept { return m_Statistics; }

	private:
		[[nodiscard]] static std::expected<void, SceneRendererError> PrepareLocalLighting(const Resources& Resources);
		[[nodiscard]] static std::expected<void, SceneRendererError> RenderDirectionalCascades(const Resources& Resources);
		[[nodiscard]] static std::expected<void, SceneRendererError> RenderSpotlights(const Resources& Resources, Statistics& Statistics);
		[[nodiscard]] static std::expected<void, SceneRendererError> RenderPointLights(const Resources& Resources, Statistics& Statistics);

		Statistics m_Statistics;
	};
} // namespace PulseForge
