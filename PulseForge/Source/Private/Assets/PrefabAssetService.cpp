#include "Core/PulseForgePCH.h"
#include "Assets/PrefabAssetService.h"

#include "Assets/AssetOperations.h"
#include "Assets/AssetPathResolver.h"
#include "Assets/PrefabSerializer.h"

#include <span>
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

	std::expected<AssetRecord, PrefabAssetError> PrefabAssetService::Create(
		AssetRegistry& Registry,
		const std::filesystem::path& ProjectRoot,
		const std::filesystem::path& ProjectRelativePath,
		const Scene& Source,
		const Entity& Root)
	{
		if (ProjectRelativePath.extension() != ".prefab")
		{
			return std::unexpected(PrefabAssetError{
				PrefabAssetErrorCode::UnsupportedAssetType,
				{},
				"New prefab assets must use the .prefab extension" });
		}

		const auto Serialized = PrefabSerializer::Serialize(Source, Root);
		if (!Serialized)
		{
			return std::unexpected(PrefabAssetError{
				PrefabAssetErrorCode::SerializationFailed,
				{},
				Serialized.error().Message });
		}

		const std::span<const char> Characters(Serialized->data(), Serialized->size());
		const auto Created = AssetOperations::CreateAssetFromBytes(
			Registry,
			ProjectRoot,
			std::as_bytes(Characters),
			ProjectRelativePath);
		if (!Created)
		{
			return std::unexpected(PrefabAssetError{
				PrefabAssetErrorCode::AssetOperationFailed,
				{},
				Created.error().Message });
		}
		return *Created;
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
