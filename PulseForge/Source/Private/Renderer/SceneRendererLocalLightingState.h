#pragma once

#include "Renderer/SceneRenderer.h"

#include <array>
#include <memory>
#include <span>

namespace PulseForge
{
	class SceneRenderer::LocalLightingState final
	{
	public:
		struct SelectedLight final
		{
			LocalLightRelevance Relevance;
			LocalLightGpuData Data;
			UUID Entity;
			const SceneSpotLight* Spot = nullptr;
			const ScenePointLight* Point = nullptr;
		};

		struct Plan final
		{
			bool RequiresSpotlightResources = false;
			bool RequiresPointShadowResources = false;
		};

		LocalLightingState();

		[[nodiscard]] std::expected<Plan, SceneRendererError> BuildPlan(const SceneRenderSnapshot* Snapshot);
		[[nodiscard]] std::expected<void, SceneRendererError> Commit();

		[[nodiscard]] std::span<const SelectedLight> SelectedLights() const noexcept;
		[[nodiscard]] const SpotlightShadowSlotOwners& SpotlightSlotOwners() const noexcept;
		[[nodiscard]] const PointShadowSlotOwners& PointSlotOwners() const noexcept;
		[[nodiscard]] const std::array<glm::mat4, MaxSpotlightShadowMapCount>& SpotlightMatrices() const noexcept;
		[[nodiscard]] const std::array<glm::vec4, MaxSpotlightShadowMapCount>& SpotlightParameters() const noexcept;
		[[nodiscard]] const std::array<std::array<glm::mat4, 6>, MaxPointLightShadowCount>& PointMatrices() const noexcept;
		[[nodiscard]] const std::array<glm::vec4, MaxPointLightShadowCount>& PointParameters() const noexcept;
		[[nodiscard]] const LocalLightingConstants& Constants() const noexcept;

	private:
		struct PreparedData final
		{
			std::array<SelectedLight, MaxLocalLightCount> Selected{};
			size_t SelectedCount = 0;
			SpotlightShadowSlotOwners SpotlightOwners{};
			PointShadowSlotOwners PointOwners{};
			std::array<glm::mat4, MaxSpotlightShadowMapCount> SpotlightMatrices{};
			std::array<glm::vec4, MaxSpotlightShadowMapCount> SpotlightParameters{};
			std::array<std::array<glm::mat4, 6>, MaxPointLightShadowCount> PointMatrices{};
			std::array<glm::vec4, MaxPointLightShadowCount> PointParameters{};
			LocalLightingConstants Constants{};
		};

		struct PendingData final
		{
			std::array<SpotlightShadowCandidate, MaxLocalLightCount> SpotlightCandidates{};
			size_t SpotlightCandidateCount = 0;
			std::array<PointShadowCandidate, MaxLocalLightCount> PointCandidates{};
			size_t PointCandidateCount = 0;
			bool HasSnapshot = false;
		};

		[[nodiscard]] static SceneRendererError MakeError(std::string Message);

		std::unique_ptr<PreparedData> m_Committed;
		std::unique_ptr<PreparedData> m_Staging;
		PendingData m_Pending;
		bool m_HasPendingPlan = false;
		bool m_LoggedLocalLightOverflow = false;
		bool m_LoggedSpotlightOverflow = false;
		bool m_LoggedUnsupportedSpotlightCone = false;
		bool m_LoggedPointLightOverflow = false;
	};
} // namespace PulseForge
