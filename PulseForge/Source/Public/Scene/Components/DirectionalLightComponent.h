#pragma once

#include "Core/Core.h"

#include <expected>
#include <string>

#include <glm/vec3.hpp>

namespace PulseForge
{
	struct DirectionalLightComponent
	{
		// Linear-light RGB. Intensity is a unitless multiplier, not a photometric unit.
		glm::vec3 Color{ 1.0f };
		float Intensity = 1.0f;

		[[nodiscard]] PULSEFORGE_API std::expected<void, std::string> Validate() const;
	};
}
