#pragma once

#include "Core/Core.h"
#include "Renderer/LocalLighting.h"
#include "Scene/Components/PointLightComponent.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include <glm/glm.hpp>

namespace PulseForge
{
	inline constexpr size_t MaxPointLightShadowCount = 2;
	inline constexpr uint32_t PointLightShadowMapResolution = 512;
	inline constexpr float PointLightShadowPreferredNearPlane = 0.05f;

	enum class PointShadowCubeFace : uint8_t
	{
		PositiveX,
		NegativeX,
		PositiveY,
		NegativeY,
		PositiveZ,
		NegativeZ
	};

	struct PointShadowFaceProjection
	{
		glm::vec3 Direction{ 0.0f };
		glm::vec3 Up{ 0.0f };
		glm::mat4 View{ 1.0f };
		glm::mat4 Projection{ 1.0f };
		glm::mat4 ViewProjection{ 1.0f };
	};

	struct PointShadowProjectionSet
	{
		float NearPlane = 0.0f;
		float FarPlane = 0.0f;
		std::array<PointShadowFaceProjection, 6> Faces{};
	};

	struct PointShadowMathError
	{
		std::string Message;
	};

	struct PointShadowCandidate
	{
		UUID Entity;
		uint32_t SelectedLightIndex = 0;
	};

	using PointShadowSlotOwners = std::array<std::optional<UUID>, MaxPointLightShadowCount>;
	using PointShadowSlotAssignments = std::array<int32_t, MaxLocalLightCount>;

	[[nodiscard]] PULSEFORGE_API std::expected<PointShadowProjectionSet, PointShadowMathError>
		BuildPointShadowProjections(const glm::vec3& Position, float Range);
	[[nodiscard]] PULSEFORGE_API PointShadowCubeFace SelectPointShadowCubeFace(const glm::vec3& Direction) noexcept;
	[[nodiscard]] PULSEFORGE_API float EncodePointShadowRadialDepth(float Distance, float Range) noexcept;
	[[nodiscard]] PULSEFORGE_API bool IsPointShadowOccluded(float ReceiverDepth, float StoredDepth, float Bias) noexcept;

	// Candidates arrive in current local-light relevance order; eligible top slots are recomputed every frame.
	[[nodiscard]] inline PointShadowSlotAssignments AssignPointShadowSlots(
		std::span<const PointShadowCandidate> Candidates,
		PointShadowSlotOwners& Owners) noexcept
	{
		PointShadowSlotAssignments Assignments;
		Assignments.fill(-1);
		const size_t SelectedCandidateCount = (std::min)(Candidates.size(), Owners.size());
		for (std::optional<UUID>& Owner : Owners)
		{
			if (!Owner)
				continue;
			const bool RemainsSelected = std::ranges::any_of(Candidates.first(SelectedCandidateCount),
				[&](const PointShadowCandidate& Candidate) { return Candidate.Entity == *Owner; });
			if (!RemainsSelected)
				Owner.reset();
		}

		for (const PointShadowCandidate& Candidate : Candidates.first(SelectedCandidateCount))
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
}
