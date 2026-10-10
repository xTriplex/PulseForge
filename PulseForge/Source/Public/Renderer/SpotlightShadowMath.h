#pragma once

#include "Core/Core.h"
#include "Renderer/LocalLighting.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>

#include <glm/glm.hpp>

namespace PulseForge
{
	inline constexpr float MinSpotlightShadowNearPlane = 0.05f;
	inline constexpr float MaxSpotlightShadowOuterHalfAngleDegrees = 80.0f;
	inline constexpr size_t MaxSpotlightShadowMapCount = 4;
	inline constexpr uint32_t SpotlightShadowMapResolution = 1024;

	struct SpotlightShadowProjection
	{
		glm::mat4 View{ 1.0f };
		glm::mat4 Projection{ 1.0f };
		glm::mat4 ViewProjection{ 1.0f };
	};

	struct SpotlightShadowMathError
	{
		std::string Message;
	};

	struct SpotlightShadowCandidate
	{
		UUID Entity;
		uint32_t SelectedLightIndex = 0;
	};

	using SpotlightShadowSlotOwners = std::array<std::optional<UUID>, MaxSpotlightShadowMapCount>;
	using SpotlightShadowSlotAssignments = std::array<int32_t, MaxLocalLightCount>;

	// Candidates must be provided in selected local-light relevance order. Only the highest-ranked candidates
	// receive shadow maps. Existing physical slots are preserved for owners that remain in that selected set;
	// ineligible and lower-ranked owners are evicted. Entries without a slot use -1 (unshadowed).
	[[nodiscard]] inline SpotlightShadowSlotAssignments AssignSpotlightShadowSlots(
		std::span<const SpotlightShadowCandidate> Candidates,
		SpotlightShadowSlotOwners& Owners) noexcept
	{
		SpotlightShadowSlotAssignments Assignments;
		Assignments.fill(-1);
		const size_t SelectedCandidateCount = (std::min)(Candidates.size(), Owners.size());
		for (std::optional<UUID>& Owner : Owners)
		{
			if (!Owner)
				continue;
			const bool RemainsSelected = std::ranges::any_of(Candidates.first(SelectedCandidateCount),
				[&](const SpotlightShadowCandidate& Candidate) { return Candidate.Entity == *Owner; });
			if (!RemainsSelected)
				Owner.reset();
		}

		for (const SpotlightShadowCandidate& Candidate : Candidates.first(SelectedCandidateCount))
		{
			if (Candidate.SelectedLightIndex >= Assignments.size())
				continue;
			size_t Slot = 0;
			for (; Slot < Owners.size(); ++Slot)
				if (Owners[Slot] && *Owners[Slot] == Candidate.Entity)
					break;
			if (Slot == Owners.size())
			{
				const auto Free = std::find(Owners.begin(), Owners.end(), std::nullopt);
				if (Free == Owners.end())
					continue;
				Slot = static_cast<size_t>(std::distance(Owners.begin(), Free));
				Owners[Slot] = Candidate.Entity;
			}
			Assignments[Candidate.SelectedLightIndex] = static_cast<int32_t>(Slot);
		}
		return Assignments;
	}

	// Uses local -Z light direction, a Vulkan [0, 1] depth projection, and stable up selection.
	[[nodiscard]] PULSEFORGE_API std::expected<SpotlightShadowProjection, SpotlightShadowMathError>
		BuildSpotlightShadowProjection(
			const glm::vec3& Position,
			const glm::vec3& Direction,
			float Range,
			float OuterConeHalfAngleDegrees,
			float AspectRatio = 1.0f);

	// Maps Vulkan NDC to shadow UV/depth. Returns false when the point is outside the light frustum.
	[[nodiscard]] PULSEFORGE_API bool ProjectSpotlightShadowCoordinate(
		const glm::mat4& ViewProjection,
		const glm::vec3& WorldPosition,
		glm::vec3& UvDepth) noexcept;
}
