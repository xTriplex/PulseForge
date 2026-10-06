#pragma once

#include "Core/Core.h"

#include <cstdint>
#include <expected>
#include <string>

#include <glm/glm.hpp>

namespace PulseForge
{
	enum class CameraErrorCode : uint8_t
	{
		InvalidFieldOfView,
		InvalidClipPlanes,
		InvalidAspectRatio,
		NonFiniteProjection
	};

	struct CameraError
	{
		CameraErrorCode Code;
		std::string Message;
	};

	struct CameraComponent
	{
		// Angles are radians; projection is right-handed, -Z-forward with zero-to-one depth.
		float VerticalFieldOfViewRadians = 0.785398163f;
		float NearClipPlane = 0.1f;
		float FarClipPlane = 1000.0f;
		bool IsPrimary = false;

		[[nodiscard]] PULSEFORGE_API std::expected<void, CameraError> Validate() const;
		[[nodiscard]] PULSEFORGE_API std::expected<glm::mat4, CameraError> GetProjectionMatrix(float AspectRatio) const;
	};
}
