#pragma once

#include "Renderer/SceneRenderer.h"

#include <array>
#include <map>
#include <optional>
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
			BindingLayoutHandle SpotlightShadowLayout;
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
		[[nodiscard]] std::expected<void, SceneRendererError> EnsureBackgroundPipeline(ColorTargetFormat ColorFormat);
		[[nodiscard]] std::expected<void, SceneRendererError> EnsureShadowPipeline(
			float DepthBias, bool DoubleSided = false, WorldTransformOrientation Orientation = WorldTransformOrientation::Normal);
		[[nodiscard]] std::expected<void, SceneRendererError> EnsurePointShadowPipeline(
			bool DoubleSided, WorldTransformOrientation Orientation = WorldTransformOrientation::Normal);

		[[nodiscard]] const GraphicsPipeline* FindScenePipeline(ColorTargetFormat ColorFormat, bool AlphaBlend, bool DoubleSided,
			WorldTransformOrientation Orientation = WorldTransformOrientation::Normal) const noexcept;
		[[nodiscard]] const GraphicsPipeline* FindBackgroundPipeline(ColorTargetFormat ColorFormat) const noexcept;
		[[nodiscard]] const GraphicsPipeline* FindShadowPipeline(
			int32_t RasterBias, bool DoubleSided, WorldTransformOrientation Orientation = WorldTransformOrientation::Normal) const noexcept;
		[[nodiscard]] const GraphicsPipeline* FindPointShadowPipeline(
			bool DoubleSided, WorldTransformOrientation Orientation = WorldTransformOrientation::Normal) const noexcept;

	private:
		[[nodiscard]] std::expected<VertexLayoutDesc, SceneRendererError> BuildAlphaTestedShadowVertexLayout(bool PointLightShadow) const;
		[[nodiscard]] std::expected<void, SceneRendererError> RequireInitialized() const;
		[[nodiscard]] static SceneRendererError MakeResourceError(std::string Message);

		RenderDevice& m_Runtime;
		std::optional<Resources> m_Resources;
		std::optional<VertexLayoutDesc> m_VertexLayout;
		std::map<std::tuple<int32_t, bool, WorldTransformOrientation>, GraphicsPipelineHandle> m_ShadowPipelines;
		std::array<GraphicsPipelineHandle, 4> m_PointShadowPipelines;
		std::map<std::tuple<ColorTargetFormat, bool, bool, WorldTransformOrientation>, GraphicsPipelineHandle> m_ScenePipelines;
		std::map<ColorTargetFormat, GraphicsPipelineHandle> m_BackgroundPipelines;
	};
} // namespace PulseForge
