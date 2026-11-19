#pragma once

#include "Renderer/SceneRenderer.h"

#include <array>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>

namespace PulseForge
{
	class SceneRendererPipelineCache final
	{
	public:
		struct Resources final
		{
			ShaderHandle SceneVertexShader;
			ShaderHandle SceneFragmentShader;
			ShaderHandle BackgroundVertexShader;
			ShaderHandle BackgroundFragmentShader;
			ShaderHandle ShadowVertexShader;
			ShaderHandle ShadowFragmentShader;
			ShaderHandle PointShadowVertexShader;
			ShaderHandle PointShadowFragmentShader;
			BindingLayoutHandle SceneMaterialLayout;
			BindingLayoutHandle DirectionalShadowLayout;
			BindingLayoutHandle AmbientOcclusionLayout;
			BindingLayoutHandle PointLightShadowLayout;
			BindingLayoutHandle ShadowObjectLayout;
			BindingLayoutHandle PointShadowObjectLayout;
		};

		explicit SceneRendererPipelineCache(RenderDevice& Runtime) noexcept;

		[[nodiscard]] std::expected<void, SceneRendererError> Initialize(Resources PipelineResources);
		[[nodiscard]] static bool HasSameVertexLayout(const VertexLayoutDesc& Left, const VertexLayoutDesc& Right) noexcept;
		[[nodiscard]] std::expected<void, SceneRendererError> SetVertexLayout(const VertexLayoutDesc& Layout);
		[[nodiscard]] const std::optional<VertexLayoutDesc>& GetVertexLayout() const noexcept { return m_VertexLayout; }

		[[nodiscard]] std::expected<void, SceneRendererError> EnsureScenePipeline(ColorTargetFormat ColorFormat, bool AlphaBlend = false,
			bool DoubleSided = false, WorldTransformOrientation Orientation = WorldTransformOrientation::Normal);
		[[nodiscard]] std::expected<void, SceneRendererError> EnsureGraphScenePipeline(
			ColorTargetFormat ColorFormat,
			std::string_view RecipeFingerprint,
			ShaderHandle FragmentShader,
			BindingLayoutHandle MaterialLayout,
			bool AlphaBlend = false,
			bool DoubleSided = false,
			WorldTransformOrientation Orientation = WorldTransformOrientation::Normal);
		[[nodiscard]] std::expected<void, SceneRendererError> EnsureBackgroundPipeline(ColorTargetFormat ColorFormat);
		[[nodiscard]] std::expected<void, SceneRendererError> EnsureShadowPipeline(
			float DepthBias, bool DoubleSided = false, WorldTransformOrientation Orientation = WorldTransformOrientation::Normal);
		[[nodiscard]] std::expected<void, SceneRendererError> EnsurePointShadowPipeline(
			bool DoubleSided, WorldTransformOrientation Orientation = WorldTransformOrientation::Normal);

		[[nodiscard]] const GraphicsPipeline* FindScenePipeline(ColorTargetFormat ColorFormat, bool AlphaBlend, bool DoubleSided,
			WorldTransformOrientation Orientation = WorldTransformOrientation::Normal) const noexcept;
		[[nodiscard]] const GraphicsPipeline* FindGraphScenePipeline(ColorTargetFormat ColorFormat, std::string_view RecipeFingerprint,
			bool AlphaBlend, bool DoubleSided,
			WorldTransformOrientation Orientation) const noexcept;
		[[nodiscard]] size_t GetGraphScenePipelineCount() const noexcept { return m_GraphScenePipelines.size(); }
		[[nodiscard]] uint64_t GetGraphScenePipelineCreations() const noexcept { return m_GraphScenePipelineCreations; }
		[[nodiscard]] uint64_t GetGraphScenePipelineEvictions() const noexcept { return m_GraphScenePipelineEvictions; }
		[[nodiscard]] const GraphicsPipeline* FindBackgroundPipeline(ColorTargetFormat ColorFormat) const noexcept;
		[[nodiscard]] const GraphicsPipeline* FindShadowPipeline(
			int32_t RasterBias, bool DoubleSided, WorldTransformOrientation Orientation = WorldTransformOrientation::Normal) const noexcept;
		[[nodiscard]] const GraphicsPipeline* FindPointShadowPipeline(
			bool DoubleSided, WorldTransformOrientation Orientation = WorldTransformOrientation::Normal) const noexcept;

	private:
		[[nodiscard]] std::expected<VertexLayoutDesc, SceneRendererError> BuildAlphaTestedShadowVertexLayout(bool PointLightShadow) const;
		[[nodiscard]] std::expected<void, SceneRendererError> RequireInitialized() const;
		[[nodiscard]] static SceneRendererError MakeResourceError(std::string Message);
		struct CachedGraphPipeline final
		{
			GraphicsPipelineHandle Pipeline;
			uint64_t LastUsed = 0;
		};

		RenderDevice& m_Runtime;
		std::optional<Resources> m_Resources;
		std::optional<VertexLayoutDesc> m_VertexLayout;
		std::map<std::tuple<int32_t, bool, WorldTransformOrientation>, GraphicsPipelineHandle> m_ShadowPipelines;
		std::array<GraphicsPipelineHandle, 4> m_PointShadowPipelines;
		std::map<std::tuple<ColorTargetFormat, bool, bool, WorldTransformOrientation>, GraphicsPipelineHandle> m_ScenePipelines;
		// Every graph product uses the same versioned space-3 descriptor ABI. Key by that compatibility
		// contract rather than a per-material BindingLayout object's address.
		std::map<std::tuple<std::string, ColorTargetFormat, bool, bool, WorldTransformOrientation>, CachedGraphPipeline>
			m_GraphScenePipelines;
		uint64_t m_GraphPipelineUseSequence = 0;
		uint64_t m_GraphScenePipelineCreations = 0;
		uint64_t m_GraphScenePipelineEvictions = 0;
		std::map<ColorTargetFormat, GraphicsPipelineHandle> m_BackgroundPipelines;
	};
} // namespace PulseForge
