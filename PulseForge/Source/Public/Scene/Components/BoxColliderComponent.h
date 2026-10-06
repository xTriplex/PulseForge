#pragma once

#include "Core/Core.h"

#include <expected>
#include <glm/glm.hpp>
#include <string>

namespace PulseForge
{
	enum class BoxColliderErrorCode : uint8_t
	{
		InvalidHalfExtents
	};

	struct BoxColliderError
	{
		BoxColliderErrorCode Code;
		std::string Message;
	};

	struct BoxColliderComponent
	{
		glm::vec3 HalfExtents{ 0.5f };

		[[nodiscard]] PULSEFORGE_API std::expected<void, BoxColliderError> Validate() const;
	};
}
