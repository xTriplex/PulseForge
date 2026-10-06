#include "Core/PulseForgePCH.h"
#include "Scene/Components/BoxColliderComponent.h"

#include <cmath>

namespace PulseForge
{
	std::expected<void, BoxColliderError> BoxColliderComponent::Validate() const
	{
		if (!std::isfinite(HalfExtents.x) || !std::isfinite(HalfExtents.y) || !std::isfinite(HalfExtents.z) ||
			HalfExtents.x <= 0.0f || HalfExtents.y <= 0.0f || HalfExtents.z <= 0.0f)
		{
			return std::unexpected(BoxColliderError{
				BoxColliderErrorCode::InvalidHalfExtents,
				"Box collider half-extents must be finite and greater than zero" });
		}
		return {};
	}
}
