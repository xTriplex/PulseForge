#pragma once

#include "Core/Core.h"

#include <array>
#include <expected>
#include <string>

#include <glm/glm.hpp>

namespace PulseForge
{
	struct DirectionalShadowCascade
	{
		glm::mat4 ViewProjection{ 1.0f };
		float NearDistance = 0.0f;
		float FarDistance = 0.0f;
	};

	struct DirectionalShadowCascadeSet
	{
		std::array<DirectionalShadowCascade, 4> Cascades;
		float ShadowDistance = 0.0f;
	};

	struct DirectionalShadowMathError
	{
		std::string Message;
	};

	// Uses practical logarithmic/uniform splits (lambda 0.65), a stable square fit, and 1024-texel snapping.
	[[nodiscard]] PULSEFORGE_API std::expected<DirectionalShadowCascadeSet, DirectionalShadowMathError>
		BuildDirectionalShadowCascades(
			const glm::mat4& View,
			const glm::mat4& Projection,
			float NearClipPlane,
			float FarClipPlane,
			float ShadowDistance,
			const glm::vec3& LightRayDirection,
			uint32_t Resolution = 1024);
}
