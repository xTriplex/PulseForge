#pragma once

#include "Assets/AssetID.h"
#include "Core/Core.h"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>

namespace PulseForge
{
	enum class AssetMetadataErrorCode : uint8_t
	{
		InvalidPath,
		SourceAssetMissing,
		MetadataAlreadyExists,
		FileOpenFailed,
		FileReadFailed,
		InvalidDocument,
		UnsupportedVersion,
		InvalidIdentifier,
		UUIDGenerationFailed,
		FileWriteFailed,
		DestinationAssetMissing
	};

	struct AssetMetadataError
	{
		AssetMetadataErrorCode Code;
		std::filesystem::path Path;
		std::string Message;
	};

	struct AssetMetadata
	{
		AssetID ID;
		uint32_t Version = 1;
	};

	class PULSEFORGE_API AssetMetadataSerializer final
	{
	public:
		static constexpr uint32_t CurrentVersion = 1;

		[[nodiscard]] static std::filesystem::path GetSidecarPath(const std::filesystem::path& SourceAssetPath);
		[[nodiscard]] static std::expected<AssetMetadata, AssetMetadataError> LoadFromFile(const std::filesystem::path& SidecarPath);
		// Use only when importing or duplicating a genuinely new asset. Existing assets are never repaired automatically.
		[[nodiscard]] static std::expected<AssetMetadata, AssetMetadataError> CreateForNewAsset(
			const std::filesystem::path& SourceAssetPath);
		// Both files must exist. Copies source settings to the new asset while assigning it a fresh UUID.
		[[nodiscard]] static std::expected<AssetMetadata, AssetMetadataError> CreateDuplicateForNewAsset(
			const std::filesystem::path& SourceAssetPath,
			const std::filesystem::path& DuplicateAssetPath);
	};
}
