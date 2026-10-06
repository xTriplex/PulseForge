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
		RecoveryRequired,
		RegistryRefreshFailed,
		ImportSourceInvalid,
		AssetNotFound
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
		// Destination paths are project-relative. Existing source assets are selected by UUID and resolved through the registry.
		// Operations validate and refresh the supplied registry; move preserves identity and duplicate creates a new UUID.
		// Import copies an external regular file into Assets and creates a fresh sidecar UUID.
		[[nodiscard]] static std::expected<AssetRecord, AssetOperationError> ImportFile(
			AssetRegistry& Registry,
			const std::filesystem::path& ProjectRoot,
			const std::filesystem::path& SourceFile,
			const std::filesystem::path& DestinationPath);
		[[nodiscard]] static std::expected<AssetRecord, AssetOperationError> Move(
			AssetRegistry& Registry,
			const std::filesystem::path& ProjectRoot,
			const AssetID& SourceAsset,
			const std::filesystem::path& DestinationPath);
		[[nodiscard]] static std::expected<AssetRecord, AssetOperationError> Duplicate(
			AssetRegistry& Registry,
			const std::filesystem::path& ProjectRoot,
			const AssetID& SourceAsset,
			const std::filesystem::path& DestinationPath);
		// Permanently removes both the managed source and its sidecar. The caller owns any UI confirmation policy.
		[[nodiscard]] static std::expected<void, AssetOperationError> Delete(
			AssetRegistry& Registry,
			const std::filesystem::path& ProjectRoot,
			const AssetID& SourceAsset);
	};
}
