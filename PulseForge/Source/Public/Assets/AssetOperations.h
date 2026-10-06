#pragma once

#include "Assets/AssetRegistry.h"
#include "Core/Core.h"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>

namespace PulseForge
{
	enum class AssetOperationErrorCode : uint8_t
	{
		InvalidProject,
		InvalidPath,
		InvalidProjectAssets,
		SourceNotManaged,
		DestinationExists,
		FilesystemFailure,
		MetadataFailure,
		RecoveryRequired
	};

	struct AssetOperationError
	{
		AssetOperationErrorCode Code;
		std::filesystem::path Path;
		std::string Message;
	};

	class PULSEFORGE_API AssetOperations final
	{
	public:
		// Asset paths are project-relative (for example, Assets/Models/ship.gltf).
		// Move keeps the existing sidecar UUID; duplicate copies import settings with a fresh UUID.
		[[nodiscard]] static std::expected<AssetRecord, AssetOperationError> Move(
			const std::filesystem::path& ProjectRoot,
			const std::filesystem::path& SourcePath,
			const std::filesystem::path& DestinationPath);
		[[nodiscard]] static std::expected<AssetRecord, AssetOperationError> Duplicate(
			const std::filesystem::path& ProjectRoot,
			const std::filesystem::path& SourcePath,
			const std::filesystem::path& DestinationPath);
		// Permanently removes both the managed source and its sidecar. The caller owns any UI confirmation policy.
		[[nodiscard]] static std::expected<void, AssetOperationError> Delete(
			const std::filesystem::path& ProjectRoot,
			const std::filesystem::path& SourcePath);
	};
}
