#pragma once

#include "Core/Core.h"

#include <array>
#include <cstdint>
#include <expected>

#include <glm/glm.hpp>

namespace PulseForge
{
	inline constexpr size_t AmbientOcclusionSampleCount = 32;

	struct AmbientOcclusionKernel
	{
		std::array<glm::vec4, AmbientOcclusionSampleCount> Samples{};
	};

	[[nodiscard]] PULSEFORGE_API AmbientOcclusionKernel GenerateAmbientOcclusionKernel() noexcept;
	[[nodiscard]] PULSEFORGE_API std::array<uint32_t, 2> CalculateAmbientOcclusionExtent(
		uint32_t Width,
		uint32_t Height) noexcept;
	[[nodiscard]] PULSEFORGE_API std::expected<glm::vec3, bool> ReconstructViewPosition(
		const glm::mat4& InverseProjection,
		const glm::vec2& TextureUV,
		float Depth) noexcept;
	[[nodiscard]] PULSEFORGE_API glm::vec3 EncodeViewNormal(const glm::vec3& Normal) noexcept;
	[[nodiscard]] PULSEFORGE_API glm::vec3 DecodeViewNormal(const glm::vec3& EncodedNormal) noexcept;
	[[nodiscard]] PULSEFORGE_API float AmbientOcclusionRangeWeight(float ViewDepthDifference, float Radius) noexcept;
	[[nodiscard]] PULSEFORGE_API float ComputeSpecularOcclusion(float AmbientOcclusion, float NdotV, float Roughness) noexcept;
}
