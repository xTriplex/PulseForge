#pragma once

#include "Assets/AssetRegistry.h"
#include "Core/Core.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
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
		CommittedWithCleanupFailure,
		RegistryRefreshFailed,
		ImportSourceInvalid,
		AssetNotFound
	};

	struct AssetOperationError
	{
		AssetOperationErrorCode Code;
		std::filesystem::path Path;
		std::string Message;
		// Set only when the operation committed the asset but failed to remove auxiliary recovery data.
		std::optional<AssetRecord> CommittedAsset;
		// Identifies temporary recovery data retained after explicit cleanup failed.
		std::optional<std::filesystem::path> RecoveryPath;
	};

	class PULSEFORGE_API AssetOperations final
	{
	public:
		using RegistryRefresh = std::function<std::expected<void, AssetRegistryError>(const std::filesystem::path&)>;
		// Destination paths are project-relative. Existing source assets are selected by UUID and resolved through the registry.
		// Operations validate and refresh the supplied registry; move preserves identity and duplicate creates a new UUID.
		// Import copies an external regular file into Assets and creates a fresh sidecar UUID.
		[[nodiscard]] static std::expected<AssetRecord, AssetOperationError> ImportFile(
			AssetRegistry& Registry,
			const std::filesystem::path& ProjectRoot,
			const std::filesystem::path& SourceFile,
			const std::filesystem::path& DestinationPath);
		// Creates a new managed source file from bytes, assigns a fresh UUID sidecar, and refreshes the registry.
		// On CommittedWithCleanupFailure, the returned error includes CommittedAsset; callers must not retry creation.
		[[nodiscard]] static std::expected<AssetRecord, AssetOperationError> CreateAssetFromBytes(
			AssetRegistry& Registry,
			const std::filesystem::path& ProjectRoot,
			std::span<const std::byte> Contents,
			const std::filesystem::path& DestinationPath);
		[[nodiscard]] static std::expected<AssetRecord, AssetOperationError> Move(
			AssetRegistry& Registry,
			const std::filesystem::path& ProjectRoot,
			const AssetID& SourceAsset,
			const std::filesystem::path& DestinationPath);
#if defined(PF_DEBUG)
		// The refresh callback is available only in Debug builds so tests can deterministically exercise
		// post-move registry failure and recovery without changing normal production behavior.
		[[nodiscard]] static std::expected<AssetRecord, AssetOperationError> MoveWithRegistryRefreshForTesting(
			AssetRegistry& Registry,
			const std::filesystem::path& ProjectRoot,
			const AssetID& SourceAsset,
			const std::filesystem::path& DestinationPath,
			const RegistryRefresh& Refresh);
#endif
		// Moves a managed Assets subdirectory beneath another existing managed directory. The folder's
		// asset sidecars move with it, preserving every AssetID. Source and destination are project-relative.
		[[nodiscard]] static std::expected<std::filesystem::path, AssetOperationError> MoveFolder(
			AssetRegistry& Registry,
			const std::filesystem::path& ProjectRoot,
			const std::filesystem::path& SourceFolderPath,
			const std::filesystem::path& DestinationParentPath);
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
	private:
		[[nodiscard]] static std::expected<AssetRecord, AssetOperationError> MoveImpl(
			AssetRegistry& Registry,
			const std::filesystem::path& ProjectRoot,
			const AssetID& SourceAsset,
			const std::filesystem::path& DestinationPath,
			const RegistryRefresh& Refresh);
	};
}
