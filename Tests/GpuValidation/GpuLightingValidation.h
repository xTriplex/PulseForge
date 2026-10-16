#pragma once

#include "Renderer/RenderTarget.h"
#include "Scene/Entity.h"
#include "Scene/Scene.h"

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace PulseForge
{
	class Application;
	class Scene;
	class SceneRenderer;
} // namespace PulseForge

namespace PulseForge::GpuValidation
{
	enum class LightingValidationMode : uint8_t
	{
		LocalLights,
		SpotlightShadows,
		PointShadows
	};

	// Owns one lighting validation scenario's scene mutations, output target,
	// frame sequence, and renderer assertions. The application and renderer are
	// borrowed and must outlive this object.
	class GpuLightingValidation final
	{
	public:
		GpuLightingValidation(Application& App, std::unique_ptr<Scene> Scenario, SceneRenderer& Renderer, LightingValidationMode Mode,
			std::chrono::steady_clock::time_point StartedAt) noexcept;

		void Initialize();
		[[nodiscard]] std::expected<bool, std::string> Update();
		[[nodiscard]] std::expected<void, std::string> Render();
		void Reset() noexcept;

		[[nodiscard]] bool IsComplete() const noexcept;
		[[nodiscard]] std::string_view CompletionSummary() const noexcept;

	private:
		void InitializeLocalLightScenario();
		void InitializeSpotlightShadowScenario();
		void InitializePointShadowScenario();
		void UpdatePointShadowScenario();
		void RenderPointShadowFrame();
		void UpdateSpotlightShadowScenario();
		void RenderSpotlightShadowFrame();
		void UpdateLocalLightTransforms();
		void RenderLocalLightFrame();
		void Fail(std::string Message);

		Application& m_Application;
		std::unique_ptr<Scene> m_Scene;
		SceneRenderer& m_SceneRenderer;
		LightingValidationMode m_Mode;
		std::chrono::steady_clock::time_point m_StartedAt;
		std::vector<Entity> m_LocalLightEntities;
		std::vector<Entity> m_SpotlightShadowEntities;
		std::vector<Entity> m_PointShadowEntities;
		RenderTargetHandle m_OutputTarget;
		uint32_t m_LocalLightFrames = 0;
		uint32_t m_SpotShadowFrames = 0;
		uint32_t m_PointShadowFrames = 0;
		uint64_t m_FirstSpotShadowHdrGeneration = 0;
		bool m_Succeeded = true;
		bool m_ScenePrepared = false;
		bool m_PointShadowScenarioUpdated = false;
		std::optional<std::string> m_Failure;
	};
} // namespace PulseForge::GpuValidation
