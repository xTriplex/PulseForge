#pragma once

#include "Core/Core.h"

#include <cstdint>
#include <string>

namespace PulseForge
{
	enum class SceneErrorCode : uint8_t
	{
		InvalidEntity,
		ForeignEntity,
		NilUUID,
		DuplicateUUID,
		UUIDGenerationFailed,
		InvalidTransform,
		ParentCycle,
		StorageFailure,
		MissingComponent,
		InvalidCamera,
		InvalidAssetReference,
		InvalidPhysicsComponent
	};

	struct SceneError
	{
		SceneErrorCode Code;
		std::string Message;
	};
}
