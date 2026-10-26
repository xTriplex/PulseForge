#pragma once

#include "Renderer/SceneRenderer.h"
#include "Renderer/ScreenSpaceAmbientOcclusion.h"

#include <functional>
#include <utility>
#include <vector>

namespace PulseForge
{
	class MeshAssetCache;

	class SceneRenderer::AmbientOcclusionPass final
	{
	public:
		using MaterialResolver = std::function<std::expected<const MaterialAssetDesc*, SceneRendererError>(const SceneMeshInstance&)>;
		using BindingResolver = std::function<std::expected<const BindingSet*, SceneRendererError>(const SceneMeshInstance&)>;

		struct RenderInputs final
		{
			const SceneRenderSnapshot& Snapshot;
			const std::optional<VertexLayoutDesc>& SceneVertexLayout;
			const BindingLayoutHandle& SceneMaterialLayout;
			const PreparedMeshResourceMap& PreparedMeshes;
			const Buffer& FullscreenTriangle;
			const MaterialResolver& ResolveMaterial;
			const BindingResolver& ResolveBindings;
		};

		explicit AmbientOcclusionPass(RenderDevice& Runtime) noexcept;
		[[nodiscard]] std::expected<void, SceneRendererError> Initialize(ShaderHandle PrepassVertex, ShaderHandle PrepassFragment,
			ShaderHandle EvaluationVertex, ShaderHandle EvaluationFragment, ShaderHandle BlurVertex, ShaderHandle BlurFragment);
		[[nodiscard]] std::expected<void, SceneRendererError> Render(const RenderInputs& Inputs, uint32_t Width, uint32_t Height);
		[[nodiscard]] bool SetSettings(const AmbientOcclusionSettings& Settings) noexcept;
		[[nodiscard]] const AmbientOcclusionSettings& GetSettings() const noexcept { return m_Settings; }
		[[nodiscard]] const Texture* GetFallbackOcclusionTexture() const noexcept;
		[[nodiscard]] const Texture* GetPrepassDepthTextureForValidation() const noexcept;
		[[nodiscard]] const BindingLayoutHandle& GetFinalLayout() const noexcept { return m_FinalLayout; }
		[[nodiscard]] const BindingSet* GetSceneBindingSet() const noexcept;
		void InvalidateFrame() noexcept { m_FrameAvailable = false; }

	private:
		struct alignas(16) Constants
		{
			glm::mat4 InverseProjection{ 1.0f };
			glm::mat4 Projection{ 1.0f };
			glm::vec4 OutputSize{};
			glm::vec4 HalfSize{};
			glm::vec4 Parameters{};
			AmbientOcclusionKernel Kernel;
		};
		static_assert(sizeof(Constants) == 688);

		struct alignas(16) BlurConstants
		{
			glm::mat4 InverseProjection{ 1.0f };
			glm::vec4 OutputSize{};
			glm::vec4 HalfSize{};
			glm::vec4 Direction{};
		};
		static_assert(sizeof(BlurConstants) == 112);

		struct alignas(16) ObjectConstants
		{
			glm::mat4 ViewProjectionModel{ 1.0f };
			glm::mat4 ViewNormalTransform{ 1.0f };
		};
		static_assert(sizeof(ObjectConstants) == 128);

		[[nodiscard]] std::expected<void, SceneRendererError> EnsureResources(uint32_t Width, uint32_t Height);
		[[nodiscard]] std::expected<void, SceneRendererError> EnsurePipelines(
			const BindingLayoutHandle& SceneMaterialLayout, const std::optional<VertexLayoutDesc>& SceneVertexLayout);

		RenderDevice& m_Runtime;
		ShaderHandle m_PrepassVertexShader;
		ShaderHandle m_PrepassFragmentShader;
		ShaderHandle m_EvaluationVertexShader;
		ShaderHandle m_EvaluationFragmentShader;
		ShaderHandle m_BlurVertexShader;
		ShaderHandle m_BlurFragmentShader;
		SamplerHandle m_PointSampler;
		SamplerHandle m_LinearSampler;
		BindingLayoutHandle m_FinalLayout;
		BindingLayoutHandle m_EvaluationLayout;
		BindingLayoutHandle m_BlurLayout;
		BindingLayoutHandle m_PrepassLayout;
		BindingSetHandle m_FinalBindings;
		BindingSetHandle m_FallbackBindings;
		BindingSetHandle m_EvaluationBindings;
		std::array<BindingSetHandle, 2> m_BlurBindings;
		BindingSetHandle m_PrepassBindings;
		BufferHandle m_ParametersBuffer;
		BufferHandle m_BlurParametersBuffer;
		BufferHandle m_ObjectConstantsBuffer;
		TextureHandle m_FallbackOcclusionTexture;
		std::array<RenderTargetHandle, 4> m_Targets;
		std::array<GraphicsPipelineHandle, 4> m_PrepassPipelines;
		GraphicsPipelineHandle m_EvaluationPipeline;
		GraphicsPipelineHandle m_BlurPipeline;
		AmbientOcclusionSettings m_Settings;
		AmbientOcclusionKernel m_Kernel;
		uint32_t m_Width = 0;
		uint32_t m_Height = 0;
		bool m_FrameAvailable = false;
	};
} // namespace PulseForge
