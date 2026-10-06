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
		}
		return Issues;
	}
}
