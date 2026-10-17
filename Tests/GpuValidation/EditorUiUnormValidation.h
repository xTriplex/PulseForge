#pragma once

#include "Renderer/Binding.h"
#include "Renderer/Buffer.h"
#include "Renderer/Graphics.h"
#include "Renderer/RenderTarget.h"
#include "Renderer/Texture.h"

#include <cstdint>
#include <expected>
#include <string>

namespace PulseForge
{
	class Application;
}

namespace PulseForge::GpuValidation
{
	// Owns the validation scenario's GPU resources across stable extents and resize.
	class EditorUiUnormValidation final
	{
	public:
		void Initialize(Application& App, const BindingLayoutHandle& BindingLayout, const Sampler& Sampler);
		[[nodiscard]] std::expected<void, std::string> RenderFrame(Application& App);
		void Reset() noexcept;
		[[nodiscard]] bool IsComplete() const noexcept { return m_EditorUiFrames == 3 && m_EditorUiTargetCreations == 2; }
		[[nodiscard]] uint32_t CompletedFrames() const noexcept { return m_EditorUiFrames; }
		[[nodiscard]] uint32_t TargetCreations() const noexcept { return m_EditorUiTargetCreations; }

	private:
		void EnsureTargets(Application& App, uint32_t Width, uint32_t Height);

		BindingLayoutHandle m_BindingLayout;
		// The harness owns the sampler and resets this scenario before releasing it.
		const Sampler* m_Sampler = nullptr;
		ShaderHandle m_EditorUiVertexShader;
		ShaderHandle m_EditorUiFragmentShader;
		ShaderHandle m_EditorUiPresentationVertexShader;
		ShaderHandle m_EditorUiPresentationFragmentShader;
		TextureHandle m_EditorUiFontTexture;
		BindingSetHandle m_EditorUiDrawBindingSet;
		BindingSetHandle m_EditorUiBindingSet;
		GraphicsPipelineHandle m_EditorUiPipeline;
		GraphicsPipelineHandle m_EditorUiPresentationPipeline;
		RenderTargetHandle m_EditorUiSrgbTarget;
		RenderTargetHandle m_EditorUiUnormTarget;
		BufferHandle m_EditorUiVertexBuffer;
		BufferHandle m_EditorUiFullscreenBuffer;
		const Texture* m_EditorUiFirstTexture = nullptr;
		uint32_t m_EditorUiFrames = 0;
		uint32_t m_EditorUiTargetCreations = 0;
		uint32_t m_EditorUiWidth = 0;
		uint32_t m_EditorUiHeight = 0;
	};
} // namespace PulseForge::GpuValidation
