#include "Core/PulseForgePCH.h"
#include "Scene/Components/PointLightComponent.h"

#include <cmath>

namespace PulseForge
{
	std::expected<void, std::string> PointLightComponent::Validate() const
	{
		if (!std::isfinite(Color.r) || !std::isfinite(Color.g) || !std::isfinite(Color.b) ||
			Color.r < 0.0f || Color.g < 0.0f || Color.b < 0.0f)
			return std::unexpected("Point-light color must contain finite non-negative linear RGB values");
		if (!std::isfinite(Intensity) || Intensity < 0.0f || Intensity > 1000000.0f)
			return std::unexpected("Point-light intensity must be finite and between 0 and 1000000 (unitless)");
		if (!std::isfinite(Range) || Range <= 0.0f || Range > 10000.0f)
			return std::unexpected("Point-light range must be finite, positive, and at most 10000 world units");
		return {};
	}
}
