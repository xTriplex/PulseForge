#pragma once

#include "Assets/AssetID.h"
#include "Core/Core.h"

#include <expected>
#include <string>

namespace PulseForge
{
	struct EnvironmentLightComponent
	{
		AssetID HdrImage;
		// Unitless linear-radiance multiplier. Environment orientation comes from entity world rotation.
		float Intensity = 1.0f;

		[[nodiscard]] PULSEFORGE_API std::expected<void, std::string> Validate() const;
	};
}
