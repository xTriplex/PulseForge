#pragma once

#include "Assets/AssetRegistry.h"
#include "Assets/MaterialAsset.h"

#include <expected>
#include <filesystem>

namespace PulseForge
{
	class PULSEFORGE_API MaterialAssetService final
	{
	public:
		// A CommittedWithCleanupFailure error carries the created record in CommittedAsset; do not retry it.
		[[nodiscard]] static std::expected<AssetRecord, MaterialAssetError> Create(
			AssetRegistry& Registry,
			const std::filesystem::path& ProjectRoot,
			const std::filesystem::path& ProjectRelativePath,
			const MaterialAssetDesc& Material);
		[[nodiscard]] static std::expected<MaterialAssetDesc, MaterialAssetError> Load(
			const AssetID& MaterialAsset,
			const std::filesystem::path& ProjectRoot,
			const AssetRegistry& Registry);
		[[nodiscard]] static std::expected<MaterialAssetSourceSnapshot, MaterialAssetError> LoadSourceSnapshot(
			const AssetID& MaterialAsset,
			const std::filesystem::path& ProjectRoot,
			const AssetRegistry& Registry);
		[[nodiscard]] static std::expected<void, MaterialAssetError> Save(
			const AssetID& MaterialAsset,
			const std::filesystem::path& ProjectRoot,
			const AssetRegistry& Registry,
			const MaterialAssetDesc& Material);
		// The source-byte comparison is an observation before atomic publication, not a lock against arbitrary external writers.
		[[nodiscard]] static std::expected<std::string, MaterialAssetError> SaveIfSourceUnchanged(
			const AssetID& MaterialAsset,
			const std::filesystem::path& ProjectRoot,
			const AssetRegistry& Registry,
			const MaterialAssetDesc& Material,
			std::string_view ExpectedSourceBytes);
	};
}
