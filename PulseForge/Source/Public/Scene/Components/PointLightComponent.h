#pragma once

#include "Core/Core.h"

#include <expected>
#include <string>

#include <glm/vec3.hpp>

namespace PulseForge
{
	inline constexpr float MaxPointLightShadowSoftness = 4.0f;
	inline constexpr float MaxPointLightShadowBias = 0.05f;
	inline constexpr float MaxPointLightShadowNormalBias = 1.0f;

	struct PointLightComponent
	{
		// Linear RGB and unitless radiance multiplier; this is not a photometric unit.
		glm::vec3 Color{ 1.0f };
		float Intensity = 10.0f;
		float Range = 10.0f;
		bool CastShadows = false;
		// Normalized linear radial-depth bias (fraction of the light range).
		float ShadowBias = 0.001f;
		// Receiver offset in world units, weighted by grazing incidence.
		float ShadowNormalBias = 0.025f;
		// Angular PCF sample radius measured in cubemap texels.
		float ShadowSoftness = 1.5f;

		[[nodiscard]] PULSEFORGE_API std::expected<void, std::string> Validate() const;
	};
}
