#pragma once

#include "Assets/AssetMetadata.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace PulseForge
{
	enum class AssetRegistryIssueCode : uint8_t
	{
		InvalidProjectRoot,
		MissingAssetDirectory,
		AssetScanFailed,
		UnsupportedEntry,
		MissingMetadata,
		InvalidMetadata,
		MissingSourceAsset,
		DuplicateAssetID
	};

	struct AssetRegistryIssue
	{
		AssetRegistryIssueCode Code;
		std::filesystem::path ProjectRelativePath;
		std::filesystem::path RelatedProjectRelativePath;
		std::string Message;
	};

	struct AssetRegistryError
	{
		std::vector<AssetRegistryIssue> Issues;
	};

	struct AssetRecord
	{
		AssetID ID;
		std::filesystem::path ProjectRelativePath;
	};

	class PULSEFORGE_API AssetRegistry final
	{
	public:
		// Scans <ProjectRoot>/Assets. The registry stores project-relative paths and is rebuilt from sidecars.
		[[nodiscard]] std::expected<void, AssetRegistryError> Rebuild(const std::filesystem::path& ProjectRoot);
		[[nodiscard]] std::optional<AssetRecord> Find(const AssetID& ID) const;
		[[nodiscard]] size_t GetAssetCount() const noexcept { return m_Assets.size(); }

	private:
		std::unordered_map<AssetID, AssetRecord, UUIDHash> m_Assets;
	};
}
