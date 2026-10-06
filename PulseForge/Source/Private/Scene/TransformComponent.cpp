#include "Core/PulseForgePCH.h"
#include "Scene/Components/TransformComponent.h"

#include <glm/gtc/matrix_transform.hpp>

namespace PulseForge
{
	glm::mat4 TransformComponent::GetLocalMatrix() const
	{
		return glm::translate(glm::mat4(1.0f), Translation) *
			glm::mat4_cast(Rotation) *
			glm::scale(glm::mat4(1.0f), Scale);
	}
}
