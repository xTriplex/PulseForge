#pragma once

#include "Core/Core.h"

#include <expected>
#include <string>

#include <glm/vec3.hpp>

namespace PulseForge
{
	struct SpotLightComponent
	{
		// Linear RGB and unitless radiance multiplier; this is not a photometric unit.
		glm::vec3 Color{ 1.0f };
		float Intensity = 25.0f;
		float Range = 15.0f;
		// Cone half-angles in degrees. The light points along local -Z.
		float InnerConeAngleDegrees = 15.0f;
		float OuterConeAngleDegrees = 25.0f;

		[[nodiscard]] PULSEFORGE_API std::expected<void, std::string> Validate() const;
	};
}
