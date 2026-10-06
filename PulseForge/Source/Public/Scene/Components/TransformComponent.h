#pragma once

#include "Core/Core.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace PulseForge
{
	struct TransformComponent
	{
		glm::vec3 Translation{ 0.0f };
		glm::quat Rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
		glm::vec3 Scale{ 1.0f };

		[[nodiscard]] PULSEFORGE_API glm::mat4 GetLocalMatrix() const;
	};
}
