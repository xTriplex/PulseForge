#pragma once

#include "Core/Core.h"

#include <expected>
#include <glm/glm.hpp>
#include <string>

namespace PulseForge
{
	enum class ColliderShape : uint8_t
	{
		Box,
		Sphere,
		Capsule
	};

	enum class BoxColliderErrorCode : uint8_t
	{
		InvalidShape,
		InvalidDimensions,
		InvalidCollisionCategory
	};

	struct BoxColliderError
	{
		BoxColliderErrorCode Code;
		std::string Message;
	};

	struct BoxColliderComponent
	{
		// Kept first so existing BoxColliderComponent{{x, y, z}} construction remains source-compatible.
		glm::vec3 HalfExtents{ 0.5f };
		ColliderShape Shape = ColliderShape::Box;
		float SphereRadius = 0.5f;
		float CapsuleRadius = 0.35f;
		// Total capsule height, including both spherical caps. The capsule's local axis is +Y.
		float CapsuleHeight = 1.8f;
		bool IsSensor = false;
		uint32_t CollisionCategory = 1;
		uint32_t CollisionMask = 0xffffffffu;

		[[nodiscard]] PULSEFORGE_API std::expected<void, BoxColliderError> Validate() const;
	};
}
