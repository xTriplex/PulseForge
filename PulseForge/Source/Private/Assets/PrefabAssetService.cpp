#include "Core/PulseForgePCH.h"
#include "Assets/PrefabAssetService.h"

#include "Assets/AssetPathResolver.h"
#include "Assets/PrefabSerializer.h"

#include <utility>

namespace PulseForge
{
	namespace
	{
		std::expected<std::filesystem::path, PrefabAssetError> ResolvePrefabPath(
			const AssetID& PrefabAsset,
			const std::filesystem::path& ProjectRoot,
			const AssetRegistry& Registry)
		{
			const auto Record = Registry.Find(PrefabAsset);
			if (!Record)
			{
				return std::unexpected(PrefabAssetError{
					PrefabAssetErrorCode::AssetNotFound,
					PrefabAsset,
					"Prefab asset UUID " + PrefabAsset.ToString() + " is not present in the project asset registry" });
			}

			if (Record->ProjectRelativePath.extension() != ".prefab")
			{
				return std::unexpected(PrefabAssetError{
					PrefabAssetErrorCode::UnsupportedAssetType,
					PrefabAsset,
					"Asset UUID " + PrefabAsset.ToString() + " does not refer to a .prefab file" });
			}

			auto ResolvedPath = AssetPathResolver::ResolveManagedSourcePath(ProjectRoot, Record->ProjectRelativePath);
			if (!ResolvedPath)
			{
				return std::unexpected(PrefabAssetError{
					PrefabAssetErrorCode::PathResolutionFailed,
					PrefabAsset,
					ResolvedPath.error().Message });
			}
			return *ResolvedPath;
		}
	}

	std::expected<Entity, PrefabAssetError> PrefabAssetService::Instantiate(
		const AssetID& PrefabAsset,
		const std::filesystem::path& ProjectRoot,
		const AssetRegistry& Registry,
		Scene& Destination)
	{
		auto Path = ResolvePrefabPath(PrefabAsset, ProjectRoot, Registry);
		if (!Path)
			return std::unexpected(std::move(Path.error()));

		auto Result = PrefabSerializer::InstantiateFromFile(*Path, Destination);
		if (!Result)
		{
			return std::unexpected(PrefabAssetError{
				PrefabAssetErrorCode::SerializationFailed,
				PrefabAsset,
				Result.error().Message });
		}
		return *Result;
	}

	std::expected<void, PrefabAssetError> PrefabAssetService::Save(
		const AssetID& PrefabAsset,
		const std::filesystem::path& ProjectRoot,
		const AssetRegistry& Registry,
		const Scene& Source,
		const Entity& Root)
	{
		auto Path = ResolvePrefabPath(PrefabAsset, ProjectRoot, Registry);
		if (!Path)
			return std::unexpected(std::move(Path.error()));

		if (auto Result = PrefabSerializer::SaveToFile(Source, Root, *Path); !Result)
		{
			return std::unexpected(PrefabAssetError{
				PrefabAssetErrorCode::SerializationFailed,
				PrefabAsset,
				Result.error().Message });
		}
		return {};
	}
}
