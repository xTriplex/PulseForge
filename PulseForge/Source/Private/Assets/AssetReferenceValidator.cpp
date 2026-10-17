#include "Assets/AssetReferenceValidator.h"
#include "Assets/AssetTypes.h"

#include <algorithm>
#include <span>
#include <string_view>

namespace PulseForge
{
	namespace
	{
		void ValidateReference(std::vector<AssetReferenceIssue>& Issues, const AssetRegistry& Registry, const Entity& Owner,
			AssetID Identifier, AssetReferenceKind Kind, AssetFileType Type)
		{
			const auto Record = Registry.Find(Identifier);
			if (!Record)
			{
				Issues.push_back({
					AssetReferenceIssueCode::MissingAsset,
					Kind,
					Owner.GetUUID(),
					Identifier
				});
				return;
			}

			if (!HasAssetTypeExtension(Record->ProjectRelativePath, Type))
			{
				Issues.push_back({
					AssetReferenceIssueCode::WrongAssetType,
					Kind,
					Owner.GetUUID(),
					Identifier
				});
			}
		}
	}

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
			if (MeshRenderer->has_value())
			{
				const MeshRendererComponent& Component = MeshRenderer->value();
				ValidateReference(Issues, Registry, CurrentEntity, Component.MeshAsset, AssetReferenceKind::Mesh, AssetFileType::Mesh);
				if (Component.MaterialAsset)
					ValidateReference(
						Issues, Registry, CurrentEntity, *Component.MaterialAsset, AssetReferenceKind::Material, AssetFileType::Material);
			}

			const auto ScriptComponent = CurrentEntity.GetScript();
			if (!ScriptComponent)
				return std::unexpected(ScriptComponent.error());
			if (ScriptComponent->has_value())
				ValidateReference(Issues, Registry, CurrentEntity, ScriptComponent->value().ScriptAsset, AssetReferenceKind::Script,
					AssetFileType::Script);

			const auto AudioSource = CurrentEntity.GetAudioSource();
			if (!AudioSource)
				return std::unexpected(AudioSource.error());
			if (AudioSource->has_value())
			{
				ValidateReference(
					Issues, Registry, CurrentEntity, AudioSource->value().AudioAsset, AssetReferenceKind::Audio, AssetFileType::Audio);
			}

			const auto EnvironmentLight = CurrentEntity.GetEnvironmentLight();
			if (!EnvironmentLight)
				return std::unexpected(EnvironmentLight.error());
			if (EnvironmentLight->has_value())
				ValidateReference(Issues, Registry, CurrentEntity, EnvironmentLight->value().HdrImage, AssetReferenceKind::Environment,
					AssetFileType::Environment);
		}
		return Issues;
	}
}
