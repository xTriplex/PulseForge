#pragma once

#include "Renderer/SceneRenderer.h"

#include <map>
#include <span>

namespace PulseForge
{
	class SceneRenderer::ToneMappingStage final
	{
	public:
		explicit ToneMappingStage(Application& Runtime) noexcept;

		[[nodiscard]] std::expected<void, SceneRendererError> Initialize(
			std::span<const std::byte> VertexBytecode,
			std::span<const std::byte> FragmentBytecode);
		[[nodiscard]] std::expected<void, SceneRendererError> Prepare(
			const Texture& HdrTexture,
			uint64_t Generation,
			ColorTargetFormat DestinationFormat,
			bool DepthAttachmentEnabled);
		[[nodiscard]] std::expected<size_t, SceneRendererError> Draw(
			const Texture& HdrTexture,
			uint64_t Generation,
			ColorTargetFormat DestinationFormat,
			bool DepthAttachmentEnabled,
			const Buffer& FullscreenTriangle);
		void InvalidateBindings() noexcept;
		[[nodiscard]] bool SetSettings(const ToneMappingSettings& Settings) noexcept;
		[[nodiscard]] const ToneMappingSettings& GetSettings() const noexcept;

	private:
		struct alignas(16) Constants
		{
			float ExposureEV = 0.0f;
			uint32_t EncodeSrgbForUnorm = 0;
			std::array<uint32_t, 2> Padding{};
		};
		static_assert(sizeof(Constants) == 16);

		[[nodiscard]] std::expected<void, SceneRendererError> EnsurePipeline(
			ColorTargetFormat ColorFormat,
			bool DepthAttachmentEnabled);
		[[nodiscard]] std::expected<void, SceneRendererError> EnsureBindings(
			const Texture& HdrTexture,
			uint64_t Generation);

		Application& m_Runtime;
		ShaderHandle m_VertexShader;
		ShaderHandle m_FragmentShader;
		SamplerHandle m_Sampler;
		BindingLayoutHandle m_BindingLayout;
		BindingSetHandle m_BindingSet;
		BufferHandle m_ConstantsBuffer;
		std::map<std::pair<ColorTargetFormat, bool>, GraphicsPipelineHandle> m_Pipelines;
		ToneMappingSettings m_Settings;
		Constants m_UploadedConstants{};
		uint64_t m_BindingGeneration = 0;
		bool m_HasUploadedConstants = false;
	};
}
