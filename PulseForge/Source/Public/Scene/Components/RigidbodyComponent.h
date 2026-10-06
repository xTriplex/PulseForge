#pragma once

#include "Core/Core.h"

#include <cstdint>
#include <expected>
#include <string>

namespace PulseForge
{
	enum class RigidbodyMotionType : uint8_t
	{
		Static,
		Dynamic
	};

	enum class RigidbodyErrorCode : uint8_t
	{
		InvalidMotionType,
		InvalidMass,
		InvalidFriction,
		InvalidRestitution
	};

	struct RigidbodyError
	{
		RigidbodyErrorCode Code;
		std::string Message;
	};

	struct RigidbodyComponent
	{
		RigidbodyMotionType MotionType = RigidbodyMotionType::Dynamic;
		float Mass = 1.0f;
		float Friction = 0.2f;
		float Restitution = 0.0f;
		bool AllowSleeping = true;

		[[nodiscard]] PULSEFORGE_API std::expected<void, RigidbodyError> Validate() const;
	};
}
