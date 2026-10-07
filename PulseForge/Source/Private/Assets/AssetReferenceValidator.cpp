#include "Core/PulseForgePCH.h"
#include "Assets/AssetReferenceValidator.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <span>
#include <string_view>

namespace PulseForge
{
	namespace
	{
		using SupportedExtensions = std::span<const std::string_view>;

		bool HasSupportedExtension(const std::filesystem::path& Path, SupportedExtensions Extensions)
		{
			std::string Extension = Path.extension().string();
			std::ranges::transform(Extension, Extension.begin(), [](unsigned char Character)
			{
				return static_cast<char>(std::tolower(Character));
			});
			return std::ranges::find(Extensions, Extension) != Extensions.end();
		}

		void ValidateReference(
			std::vector<AssetReferenceIssue>& Issues,
			const AssetRegistry& Registry,
			const Entity& Owner,
			AssetID Identifier,
			AssetReferenceKind Kind,
			SupportedExtensions Extensions)
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

			if (!HasSupportedExtension(Record->ProjectRelativePath, Extensions))
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
		static constexpr std::array MeshExtensions{ std::string_view{ ".gltf" }, std::string_view{ ".glb" } };
		static constexpr std::array MaterialExtensions{ std::string_view{ ".material" } };
		static constexpr std::array ScriptExtensions{ std::string_view{ ".lua" } };
		static constexpr std::array AudioExtensions{
			std::string_view{ ".wav" },
			std::string_view{ ".mp3" },
			std::string_view{ ".flac" },
			std::string_view{ ".ogg" }
		};

		std::vector<AssetReferenceIssue> Issues;
		for (const Entity& CurrentEntity : SceneToValidate.GetEntities())
		{
			const auto MeshRenderer = CurrentEntity.GetMeshRenderer();
			if (!MeshRenderer)
				return std::unexpected(MeshRenderer.error());
			if (MeshRenderer->has_value())
			{
				const MeshRendererComponent& Component = MeshRenderer->value();
				ValidateReference(Issues, Registry, CurrentEntity, Component.MeshAsset, AssetReferenceKind::Mesh, MeshExtensions);
				if (Component.MaterialAsset)
					ValidateReference(Issues, Registry, CurrentEntity, *Component.MaterialAsset, AssetReferenceKind::Material, MaterialExtensions);
			}

			const auto ScriptComponent = CurrentEntity.GetScript();
			if (!ScriptComponent)
				return std::unexpected(ScriptComponent.error());
			if (ScriptComponent->has_value())
				ValidateReference(
					Issues,
					Registry,
					CurrentEntity,
					ScriptComponent->value().ScriptAsset,
					AssetReferenceKind::Script,
					ScriptExtensions);

			const auto AudioSource = CurrentEntity.GetAudioSource();
			if (!AudioSource)
				return std::unexpected(AudioSource.error());
			if (AudioSource->has_value())
			{
				ValidateReference(
					Issues,
					Registry,
					CurrentEntity,
					AudioSource->value().AudioAsset,
					AssetReferenceKind::Audio,
					AudioExtensions);
			}
		}
		return Issues;
	}
}
