#include "Core/PulseForgePCH.h"
#include "Assets/MaterialAssetService.h"

#include "Assets/AssetOperations.h"
#include "Assets/AssetPathResolver.h"

#include <span>
#include <utility>

namespace PulseForge
{
	namespace
	{
		std::expected<std::filesystem::path, MaterialAssetError> ResolveMaterialPath(
			const AssetID& MaterialAsset,
			const std::filesystem::path& ProjectRoot,
			const AssetRegistry& Registry)
		{
			const auto Record = Registry.Find(MaterialAsset);
			if (!Record)
			{
				return std::unexpected(MaterialAssetError{
					MaterialAssetErrorCode::AssetNotFound,
					MaterialAsset,
					"Material asset UUID " + MaterialAsset.ToString() + " is not present in the project asset registry" });
			}
			if (Record->ProjectRelativePath.extension() != ".material")
			{
				return std::unexpected(MaterialAssetError{
					MaterialAssetErrorCode::UnsupportedAssetType,
					MaterialAsset,
					"Asset UUID " + MaterialAsset.ToString() + " does not refer to a .material file" });
			}

			auto Path = AssetPathResolver::ResolveManagedSourcePath(ProjectRoot, Record->ProjectRelativePath);
			if (!Path)
			{
				return std::unexpected(MaterialAssetError{
					MaterialAssetErrorCode::PathResolutionFailed,
					MaterialAsset,
					Path.error().Message });
			}
			return *Path;
		}
	}

	std::expected<AssetRecord, MaterialAssetError> MaterialAssetService::Create(
		AssetRegistry& Registry,
		const std::filesystem::path& ProjectRoot,
		const std::filesystem::path& ProjectRelativePath,
		const MaterialAssetDesc& Material)
	{
		if (ProjectRelativePath.extension() != ".material")
			return std::unexpected(MaterialAssetError{ MaterialAssetErrorCode::UnsupportedAssetType, {}, "New material assets must use the .material extension" });

		const auto Serialized = MaterialAssetSerializer::Serialize(Material);
		if (!Serialized)
			return std::unexpected(Serialized.error());

		const std::span<const char> Characters(Serialized->data(), Serialized->size());
		auto Created = AssetOperations::CreateAssetFromBytes(
			Registry,
			ProjectRoot,
			std::as_bytes(Characters),
			ProjectRelativePath);
		if (!Created)
		{
			MaterialAssetError Error{
				Created.error().Code == AssetOperationErrorCode::CommittedWithCleanupFailure
					? MaterialAssetErrorCode::CommittedWithCleanupFailure
					: MaterialAssetErrorCode::AssetOperationFailed,
				{},
				Created.error().Message };
			Error.CommittedAsset = Created.error().CommittedAsset;
			Error.RecoveryPath = Created.error().RecoveryPath;
			if (Error.CommittedAsset)
				Error.Asset = Error.CommittedAsset->ID;
			return std::unexpected(std::move(Error));
		}
		return *Created;
	}

	std::expected<MaterialAssetDesc, MaterialAssetError> MaterialAssetService::Load(
		const AssetID& MaterialAsset,
		const std::filesystem::path& ProjectRoot,
		const AssetRegistry& Registry)
	{
		auto Path = ResolveMaterialPath(MaterialAsset, ProjectRoot, Registry);
		if (!Path)
			return std::unexpected(std::move(Path.error()));
		auto Material = MaterialAssetSerializer::LoadFromFile(*Path);
		if (!Material)
		{
			Material.error().Asset = MaterialAsset;
			return std::unexpected(std::move(Material.error()));
		}
		return *Material;
	}

	std::expected<void, MaterialAssetError> MaterialAssetService::Save(
		const AssetID& MaterialAsset,
		const std::filesystem::path& ProjectRoot,
		const AssetRegistry& Registry,
		const MaterialAssetDesc& Material)
	{
		auto Path = ResolveMaterialPath(MaterialAsset, ProjectRoot, Registry);
		if (!Path)
			return std::unexpected(std::move(Path.error()));
		if (auto SaveResult = MaterialAssetSerializer::SaveToFile(Material, *Path); !SaveResult)
		{
			SaveResult.error().Asset = MaterialAsset;
			return std::unexpected(std::move(SaveResult.error()));
		}
		return {};
	}
}
