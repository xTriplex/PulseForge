#pragma once

#include "Core/Core.h"
#include "Scene/Entity.h"
#include "Scene/Scene.h"

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace PulseForge
{
	enum class PrefabErrorCode : uint8_t
	{
		InvalidRootEntity,
		InvalidDocument,
		UnsupportedFormat,
		UnsupportedVersion,
		SceneSerializationFailed,
		SceneOperationFailed,
		InstantiationFailed
	};

	struct PrefabError
	{
		PrefabErrorCode Code;
		std::string Message;
	};

	class PULSEFORGE_API PrefabSerializer final
	{
	public:
		// Captures the root and its descendants. A parent outside the captured subtree is omitted.
		[[nodiscard]] static std::expected<std::string, PrefabError> Serialize(const Scene& Source, const Entity& Root);
		// Creates a new subtree with fresh entity UUIDs; asset UUID references are retained.
		[[nodiscard]] static std::expected<Entity, PrefabError> Instantiate(std::string_view Data, Scene& Destination);
	};
}
