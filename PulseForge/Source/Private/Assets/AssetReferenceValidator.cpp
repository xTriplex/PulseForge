#include "Core/PulseForgePCH.h"
#include "Assets/AssetReferenceValidator.h"

namespace PulseForge
{
	std::expected<std::vector<AssetReferenceIssue>, SceneError> AssetReferenceValidator::Validate(
		const Scene& SceneToValidate,
		const AssetRegistry& Registry)
	{
		std::vector<AssetReferenceIssue> Issues;
		for (const Entity& CurrentEntity : SceneToValidate.GetEntities())
		{
			const auto MeshRenderer = CurrentEntity.GetMeshRenderer();
			if (!MeshRenderer)
				return std::unexpected(MeshRenderer.error());
			if (!MeshRenderer->has_value())
				continue;

			const AssetID& MeshAssetID = MeshRenderer->value().MeshAsset;
			if (!Registry.Find(MeshAssetID))
			{
				Issues.push_back({
					AssetReferenceIssueCode::MissingAsset,
					AssetReferenceKind::Mesh,
					CurrentEntity.GetUUID(),
					MeshAssetID
				});
			}
			if (MeshRenderer->value().MaterialAsset)
			{
				const AssetID& MaterialAssetID = *MeshRenderer->value().MaterialAsset;
				const auto MaterialRecord = Registry.Find(MaterialAssetID);
				if (!MaterialRecord)
				{
					Issues.push_back({
						AssetReferenceIssueCode::MissingAsset,
						AssetReferenceKind::Material,
						CurrentEntity.GetUUID(),
						MaterialAssetID
					});
				}
				else if (MaterialRecord->ProjectRelativePath.extension() != ".material")
				{
					Issues.push_back({
						AssetReferenceIssueCode::WrongAssetType,
						AssetReferenceKind::Material,
						CurrentEntity.GetUUID(),
						MaterialAssetID
					});
				}
			}

			const auto ScriptComponent = CurrentEntity.GetScript();
			if (!ScriptComponent)
				return std::unexpected(ScriptComponent.error());
			if (ScriptComponent->has_value())
			{
				const AssetID& ScriptAssetID = ScriptComponent->value().ScriptAsset;
				const auto ScriptRecord = Registry.Find(ScriptAssetID);
				if (!ScriptRecord)
				{
					Issues.push_back({
						AssetReferenceIssueCode::MissingAsset,
						AssetReferenceKind::Script,
						CurrentEntity.GetUUID(),
						ScriptAssetID
					});
				}
				else if (ScriptRecord->ProjectRelativePath.extension() != ".lua")
				{
					Issues.push_back({
						AssetReferenceIssueCode::WrongAssetType,
						AssetReferenceKind::Script,
						CurrentEntity.GetUUID(),
						ScriptAssetID
					});
				}
			}
		}
		return Issues;
	}
}
