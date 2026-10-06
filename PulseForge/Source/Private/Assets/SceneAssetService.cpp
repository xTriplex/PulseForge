#include "Core/PulseForgePCH.h"
#include "Assets/SceneAssetService.h"

#include "Assets/AssetOperations.h"
#include "Assets/AssetPathResolver.h"
#include "Scene/SceneSerializer.h"

#include <span>
#include <utility>

namespace PulseForge
{
	namespace
	{
		std::expected<std::filesystem::path, SceneAssetError> ResolveScenePath(
			const AssetID& SceneAsset,
			const std::filesystem::path& ProjectRoot,
			const AssetRegistry& Registry)
		{
			const auto Record = Registry.Find(SceneAsset);
			if (!Record)
			{
				return std::unexpected(SceneAssetError{
					SceneAssetErrorCode::AssetNotFound,
					SceneAsset,
					"Scene asset UUID " + SceneAsset.ToString() + " is not present in the project asset registry" });
			}

			if (Record->ProjectRelativePath.extension() != ".scene")
			{
				return std::unexpected(SceneAssetError{
					SceneAssetErrorCode::UnsupportedAssetType,
					SceneAsset,
					"Asset UUID " + SceneAsset.ToString() + " does not refer to a .scene file" });
			}

			auto ResolvedPath = AssetPathResolver::ResolveManagedSourcePath(ProjectRoot, Record->ProjectRelativePath);
			if (!ResolvedPath)
			{
				return std::unexpected(SceneAssetError{
					SceneAssetErrorCode::PathResolutionFailed,
					SceneAsset,
					ResolvedPath.error().Message });
			}
			return *ResolvedPath;
		}
	}

	std::expected<AssetRecord, SceneAssetError> SceneAssetService::Create(
		AssetRegistry& Registry,
		const std::filesystem::path& ProjectRoot,
		const std::filesystem::path& ProjectRelativePath,
		const Scene& Source)
	{
		if (ProjectRelativePath.extension() != ".scene")
		{
			return std::unexpected(SceneAssetError{
				SceneAssetErrorCode::UnsupportedAssetType,
				{},
				"New scene assets must use the .scene extension" });
		}

		const auto Serialized = SceneSerializer::Serialize(Source);
		if (!Serialized)
		{
			return std::unexpected(SceneAssetError{
				SceneAssetErrorCode::SerializationFailed,
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
			return std::unexpected(SceneAssetError{
				SceneAssetErrorCode::AssetOperationFailed,
				{},
				Created.error().Message });
		}
		return *Created;
	}

	std::expected<void, SceneAssetError> SceneAssetService::Load(
		const AssetID& SceneAsset,
		const std::filesystem::path& ProjectRoot,
		const AssetRegistry& Registry,
		Scene& Destination)
	{
		auto Path = ResolveScenePath(SceneAsset, ProjectRoot, Registry);
		if (!Path)
			return std::unexpected(std::move(Path.error()));

		if (auto LoadResult = SceneSerializer::LoadFromFile(*Path, Destination); !LoadResult)
		{
			return std::unexpected(SceneAssetError{
				SceneAssetErrorCode::SerializationFailed,
				SceneAsset,
				LoadResult.error().Message });
		}
		return {};
	}

	std::expected<void, SceneAssetError> SceneAssetService::Save(
		const AssetID& SceneAsset,
		const std::filesystem::path& ProjectRoot,
		const AssetRegistry& Registry,
		const Scene& Source)
	{
		auto Path = ResolveScenePath(SceneAsset, ProjectRoot, Registry);
		if (!Path)
			return std::unexpected(std::move(Path.error()));

		if (auto SaveResult = SceneSerializer::SaveToFile(Source, *Path); !SaveResult)
		{
			return std::unexpected(SceneAssetError{
				SceneAssetErrorCode::SerializationFailed,
				SceneAsset,
				SaveResult.error().Message });
		}
		return {};
	}
}
