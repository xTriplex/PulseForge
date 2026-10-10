#pragma once

#include "Core/Core.h"
#include "Scene/UUID.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <glm/glm.hpp>

namespace PulseForge
{
	inline constexpr size_t MaxLocalLightCount = 32;
	inline constexpr uint32_t PointLocalLightTypeOrder = 0;
	inline constexpr uint32_t SpotLocalLightTypeOrder = 1;

	// Four float4 registers; must remain byte-for-byte compatible with Triangle.hlsl LocalLightData.
	struct alignas(16) LocalLightGpuData
	{
		glm::vec4 PositionRange{};
		glm::vec4 ColorIntensity{};
		glm::vec4 DirectionInnerCos{};
		glm::vec4 OuterCosAndType{};
	};
	static_assert(sizeof(LocalLightGpuData) == 64);
	static_assert(alignof(LocalLightGpuData) == 16);
	static_assert(offsetof(LocalLightGpuData, PositionRange) == 0);
	static_assert(offsetof(LocalLightGpuData, ColorIntensity) == 16);
	static_assert(offsetof(LocalLightGpuData, DirectionInnerCos) == 32);
	static_assert(offsetof(LocalLightGpuData, OuterCosAndType) == 48);

	struct alignas(16) LocalLightingConstants
	{
		glm::uvec4 Counts{};
		std::array<LocalLightGpuData, MaxLocalLightCount> Lights{};
	};
	static_assert(sizeof(LocalLightingConstants) == 16 + MaxLocalLightCount * sizeof(LocalLightGpuData));
	static_assert(alignof(LocalLightingConstants) == 16);

	struct LocalLightRelevance
	{
		UUID Entity;
		glm::vec3 Position{ 0.0f };
		glm::vec3 Color{ 1.0f };
		float Intensity = 0.0f;
		float Range = 1.0f;
		uint32_t TypeOrder = 0;
	};

	[[nodiscard]] inline bool MatchesLocalLightIdentity(
		const LocalLightRelevance& Light,
		UUID Entity,
		uint32_t TypeOrder) noexcept
	{
		return Light.Entity == Entity && Light.TypeOrder == TypeOrder;
	}

	[[nodiscard]] inline double ComputeLocalLightRelevance(
		const LocalLightRelevance& Light,
		const glm::vec3& CameraPosition) noexcept
	{
		if (!std::isfinite(Light.Position.x) || !std::isfinite(Light.Position.y) || !std::isfinite(Light.Position.z) ||
			!std::isfinite(CameraPosition.x) || !std::isfinite(CameraPosition.y) || !std::isfinite(CameraPosition.z) ||
			!std::isfinite(Light.Color.r) || !std::isfinite(Light.Color.g) || !std::isfinite(Light.Color.b) ||
			!std::isfinite(Light.Intensity) || !std::isfinite(Light.Range) || Light.Intensity <= 0.0f || Light.Range <= 0.0f)
			return 0.0;
		const double X = static_cast<double>(Light.Position.x) - CameraPosition.x;
		const double Y = static_cast<double>(Light.Position.y) - CameraPosition.y;
		const double Z = static_cast<double>(Light.Position.z) - CameraPosition.z;
		const double Distance = std::hypot(X, Y, Z);
		const double OutsideRange = (std::max)(Distance - static_cast<double>(Light.Range), 0.0);
		const double ColorWeight = (std::max)({
			static_cast<double>(Light.Color.r), static_cast<double>(Light.Color.g), static_cast<double>(Light.Color.b) });
		return static_cast<double>(Light.Intensity) * ColorWeight * Light.Range * Light.Range /
			(1.0 + OutsideRange * OutsideRange);
	}

	[[nodiscard]] inline bool IsLocalLightMoreRelevant(
		const LocalLightRelevance& Left,
		const LocalLightRelevance& Right,
		const glm::vec3& CameraPosition) noexcept
	{
		const double LeftScore = ComputeLocalLightRelevance(Left, CameraPosition);
		const double RightScore = ComputeLocalLightRelevance(Right, CameraPosition);
		if (LeftScore != RightScore)
			return LeftScore > RightScore;
		if (Left.Entity != Right.Entity)
			return Left.Entity < Right.Entity;
		return Left.TypeOrder < Right.TypeOrder;
	}

	// Returns input indices ordered by relevance, UUID, then light type for deterministic overflow handling.
	[[nodiscard]] inline std::vector<size_t> SelectLocalLightIndices(
		std::span<const LocalLightRelevance> Lights,
		const glm::vec3& CameraPosition,
		size_t Capacity = MaxLocalLightCount)
	{
		std::vector<size_t> Indices;
		Indices.reserve(Lights.size());
		for (size_t Index = 0; Index < Lights.size(); ++Index)
			if (Lights[Index].Intensity > 0.0f)
				Indices.push_back(Index);
		const size_t SelectedCount = (std::min)(Capacity, Indices.size());
		std::partial_sort(Indices.begin(), Indices.begin() + SelectedCount, Indices.end(), [&](size_t Left, size_t Right)
		{
			return IsLocalLightMoreRelevant(Lights[Left], Lights[Right], CameraPosition);
		});
		Indices.resize(SelectedCount);
		return Indices;
	}

	[[nodiscard]] inline float EvaluateLocalLightRangeAttenuation(float Distance, float Range) noexcept
	{
		if (!std::isfinite(Distance) || !std::isfinite(Range) || Distance < 0.0f || Range <= 0.0f || Distance >= Range)
			return 0.0f;
		const float NormalizedDistance = Distance / Range;
		const float RangeWindow = (std::max)(1.0f - NormalizedDistance * NormalizedDistance *
			NormalizedDistance * NormalizedDistance, 0.0f);
		return RangeWindow * RangeWindow / (std::max)(Distance * Distance, 0.01f);
	}

	[[nodiscard]] inline float EvaluateSpotLightAngularAttenuation(
		float CosTheta,
		float InnerConeAngleDegrees,
		float OuterConeAngleDegrees) noexcept
	{
		if (!std::isfinite(CosTheta) || !std::isfinite(InnerConeAngleDegrees) ||
			!std::isfinite(OuterConeAngleDegrees) || InnerConeAngleDegrees < 0.5f ||
			OuterConeAngleDegrees <= InnerConeAngleDegrees || OuterConeAngleDegrees > 89.5f)
			return 0.0f;
		const float InnerCos = std::cos(glm::radians(InnerConeAngleDegrees));
		const float OuterCos = std::cos(glm::radians(OuterConeAngleDegrees));
		const float T = glm::clamp((CosTheta - OuterCos) / (InnerCos - OuterCos), 0.0f, 1.0f);
		return T * T * (3.0f - 2.0f * T);
	}
}
