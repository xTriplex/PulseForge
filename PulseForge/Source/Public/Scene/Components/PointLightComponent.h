#pragma once

#include "Core/Core.h"

#include <expected>
#include <string>

#include <glm/vec3.hpp>

namespace PulseForge
{
	struct PointLightComponent
	{
		// Linear RGB and unitless radiance multiplier; this is not a photometric unit.
		glm::vec3 Color{ 1.0f };
		float Intensity = 10.0f;
		float Range = 10.0f;

		[[nodiscard]] PULSEFORGE_API std::expected<void, std::string> Validate() const;
	};
}
