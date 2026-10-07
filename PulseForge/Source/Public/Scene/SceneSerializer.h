#pragma once

#include "Core/Core.h"
#include "Scene/Scene.h"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace PulseForge
{
	enum class SceneSerializationErrorCode : uint8_t
	{
		InvalidDocument,
		UnsupportedFormat,
		UnsupportedVersion,
		InvalidEntityData,
		MissingParent,
		SceneOperationFailed,
		FileOpenFailed,
		FileReadFailed,
		FileWriteFailed,
		FileReplaceFailed
	};

	struct SceneSerializationError
	{
		SceneSerializationErrorCode Code;
		std::string Message;
	};

	class PULSEFORGE_API SceneSerializer final
	{
	public:
		[[nodiscard]] static std::expected<std::string, SceneSerializationError> Serialize(const Scene& Source);
		[[nodiscard]] static std::expected<void, SceneSerializationError> Deserialize(std::string_view Data, Scene& Destination);
		// Creates an independent scene copy retaining persistent entity and asset UUIDs.
		[[nodiscard]] static std::expected<std::unique_ptr<Scene>, SceneSerializationError> Clone(const Scene& Source);
		[[nodiscard]] static std::expected<void, SceneSerializationError> SaveToFile(const Scene& Source, const std::filesystem::path& Path);
		[[nodiscard]] static std::expected<void, SceneSerializationError> LoadFromFile(const std::filesystem::path& Path, Scene& Destination);
	};
}
