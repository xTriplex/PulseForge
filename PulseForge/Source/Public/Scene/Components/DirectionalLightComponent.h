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
		bool CastShadows = true;
		float ShadowDistance = 100.0f;
		float ShadowBias = 1.0f;
		float ShadowNormalBias = 0.025f;
		// PCF filter radius in shadow-map texels; this is not a physical source radius.
		float ShadowSoftness = 1.5f;

		[[nodiscard]] PULSEFORGE_API std::expected<void, std::string> Validate() const;
	};
}
