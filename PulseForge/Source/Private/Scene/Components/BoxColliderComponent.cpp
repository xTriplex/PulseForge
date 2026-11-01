#include "Scene/Components/BoxColliderComponent.h"

#include <cmath>

namespace PulseForge
{
	std::expected<void, BoxColliderError> BoxColliderComponent::Validate() const
	{
		if (Shape != ColliderShape::Box && Shape != ColliderShape::Sphere && Shape != ColliderShape::Capsule)
			return std::unexpected(BoxColliderError{ BoxColliderErrorCode::InvalidShape, "Collider shape is unsupported" });

		const auto ValidPositive = [](float Value) { return std::isfinite(Value) && Value > 0.0f; };
		if (CollisionCategory == 0)
			return std::unexpected(BoxColliderError{
				BoxColliderErrorCode::InvalidCollisionCategory, "Collider collision category must contain at least one bit" });

		if (!ValidPositive(HalfExtents.x) || !ValidPositive(HalfExtents.y) || !ValidPositive(HalfExtents.z) ||
			!ValidPositive(SphereRadius) || !ValidPositive(CapsuleRadius) || !ValidPositive(CapsuleHeight) ||
			CapsuleHeight < 2.0f * CapsuleRadius)
		{
			return std::unexpected(BoxColliderError{
				BoxColliderErrorCode::InvalidDimensions,
				"Collider dimensions must be finite and positive; capsule total height must be at least twice its radius" });
		}
		return {};
	}
}
