#include "Core/PulseForgePCH.h"
#include "Scene/Components/SpotLightComponent.h"

#include <cmath>

namespace PulseForge
{
	std::expected<void, std::string> SpotLightComponent::Validate() const
	{
		if (!std::isfinite(Color.r) || !std::isfinite(Color.g) || !std::isfinite(Color.b) ||
			Color.r < 0.0f || Color.g < 0.0f || Color.b < 0.0f)
			return std::unexpected("Spot-light color must contain finite non-negative linear RGB values");
		if (!std::isfinite(Intensity) || Intensity < 0.0f || Intensity > 1000000.0f)
			return std::unexpected("Spot-light intensity must be finite and between 0 and 1000000 (unitless)");
		if (!std::isfinite(Range) || Range <= 0.0f || Range > 10000.0f)
			return std::unexpected("Spot-light range must be finite, positive, and at most 10000 world units");
		if (!std::isfinite(InnerConeAngleDegrees) || !std::isfinite(OuterConeAngleDegrees) ||
			InnerConeAngleDegrees < 0.5f || OuterConeAngleDegrees > 89.5f ||
			OuterConeAngleDegrees <= InnerConeAngleDegrees)
			return std::unexpected("Spot-light cone half-angles must satisfy 0.5 <= inner < outer <= 89.5 degrees");
		if (!std::isfinite(ShadowBias) || ShadowBias < 0.0f || ShadowBias > 0.02f)
			return std::unexpected("Spot-light shadow bias must be finite and between 0 and 0.02 light-space depth");
		if (!std::isfinite(ShadowNormalBias) || ShadowNormalBias < 0.0f || ShadowNormalBias > 1.0f)
			return std::unexpected("Spot-light shadow normal bias must be finite and between 0 and 1 world unit");
		if (!std::isfinite(ShadowSoftness) || ShadowSoftness < 0.0f || ShadowSoftness > 4.0f)
			return std::unexpected("Spot-light shadow softness must be finite and between 0 and 4 texels");
		return {};
	}
}
