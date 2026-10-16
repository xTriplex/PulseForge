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

		[[nodiscard]] std::expected<void, SceneRendererError> EnsureScenePipeline(
			ColorTargetFormat ColorFormat, bool AlphaBlend = false, bool DoubleSided = false);
		[[nodiscard]] std::expected<void, SceneRendererError> EnsureBackgroundPipeline(ColorTargetFormat ColorFormat);
		[[nodiscard]] std::expected<void, SceneRendererError> EnsureShadowPipeline(float DepthBias, bool DoubleSided = false);
		[[nodiscard]] std::expected<void, SceneRendererError> EnsurePointShadowPipeline(bool DoubleSided);

		[[nodiscard]] const GraphicsPipeline* FindScenePipeline(
			ColorTargetFormat ColorFormat, bool AlphaBlend, bool DoubleSided) const noexcept;
		[[nodiscard]] const GraphicsPipeline* FindBackgroundPipeline(ColorTargetFormat ColorFormat) const noexcept;
		[[nodiscard]] const GraphicsPipeline* FindShadowPipeline(int32_t RasterBias, bool DoubleSided) const noexcept;
		[[nodiscard]] const GraphicsPipeline* FindPointShadowPipeline(bool DoubleSided) const noexcept;

	private:
		[[nodiscard]] std::expected<VertexLayoutDesc, SceneRendererError> BuildAlphaTestedShadowVertexLayout(bool PointLightShadow) const;
		[[nodiscard]] std::expected<void, SceneRendererError> RequireInitialized() const;
		[[nodiscard]] static SceneRendererError MakeResourceError(std::string Message);

		RenderDevice& m_Runtime;
		std::optional<Resources> m_Resources;
		std::optional<VertexLayoutDesc> m_VertexLayout;
		std::map<std::pair<int32_t, bool>, GraphicsPipelineHandle> m_ShadowPipelines;
		std::array<GraphicsPipelineHandle, 2> m_PointShadowPipelines;
		std::map<std::tuple<ColorTargetFormat, bool, bool>, GraphicsPipelineHandle> m_ScenePipelines;
		std::map<ColorTargetFormat, GraphicsPipelineHandle> m_BackgroundPipelines;
	};
} // namespace PulseForge
