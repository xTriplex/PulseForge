#pragma once

#include "Renderer/Binding.h"
#include "Renderer/Buffer.h"
#include "Renderer/RenderTarget.h"
#include "Scene/Scene.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>

namespace PulseForge
{
	class Application;
	class Scene;
	class SceneRenderer;
} // namespace PulseForge

namespace PulseForge::GpuValidation
{
	enum class SceneOutputValidationMode
	{
		Hdr,
		ToneMapping
	};

	class GpuSceneOutputValidation final
	{
	public:
		GpuSceneOutputValidation(Application& Runtime, Scene& StartupScene, SceneRenderer& Renderer,
			const BindingLayoutHandle& SamplingLayout, const SamplerHandle& SamplingSampler, const GraphicsPipelineHandle& SamplingPipeline,
			const BufferHandle& FullscreenTriangle, SceneOutputValidationMode Mode,
			std::chrono::steady_clock::time_point StartedAt) noexcept;

		void Initialize();
		[[nodiscard]] std::expected<bool, std::string> Update();
		[[nodiscard]] std::expected<void, std::string> Render();
		[[nodiscard]] bool IsComplete() const noexcept;

	private:
		[[nodiscard]] Scene& GetScenarioScene(size_t Index);
		[[nodiscard]] std::expected<void, std::string> RenderHdrFrame();
		[[nodiscard]] std::expected<void, std::string> RenderToneMappingFrame();
		[[nodiscard]] std::expected<void, std::string> EnsureSdrValidationTarget(ColorTargetFormat Format, uint32_t Width, uint32_t Height);

		Application& m_Runtime;
		Scene& m_StartupScene;
		SceneRenderer& m_Renderer;
		const BindingLayoutHandle& m_SamplingLayout;
		const SamplerHandle& m_SamplingSampler;
		const GraphicsPipelineHandle& m_SamplingPipeline;
		const BufferHandle& m_FullscreenTriangle;
		SceneOutputValidationMode m_Mode;
		std::chrono::steady_clock::time_point m_StartedAt;
		Scene m_EmptyScene;
		Scene m_EnvironmentOnlyScene;
		Scene m_GeometryOnlyScene;
		Scene m_DirectionalOnlyScene;
		RenderTargetHandle m_SdrTarget;
		BindingSetHandle m_SamplingBindingSet;
		const Texture* m_FirstHdrTexture = nullptr;
		const Texture* m_ResizedHdrTexture = nullptr;
		const Texture* m_BoundHdrTexture = nullptr;
		const Texture* m_BoundOutputTexture = nullptr;
		uint32_t m_SceneCompletedFrames = 0;
		uint32_t m_ToneMappingFrame = 0;
		size_t m_ScenarioIndex = 0;
		uint64_t m_SwapchainHdrGeneration = 0;
		uint64_t m_SrgbHdrGeneration = 0;
		uint64_t m_ResizedHdrGeneration = 0;
		const std::array<const char*, 4> m_ScenarioNames = { "empty", "environment-only", "geometry-only", "directional-only" };
		bool m_ScenePrepared = false;
	};
} // namespace PulseForge::GpuValidation
