#include "Assets/PrefabSerializer.h"

#include "Scene/Components/AudioListenerComponent.h"
#include "Scene/Components/AudioSourceComponent.h"
#include "Scene/Components/DirectionalLightComponent.h"
#include "Scene/Components/EnvironmentLightComponent.h"
#include "Scene/Components/MeshRendererComponent.h"
#include "Scene/Components/ScriptComponent.h"
#include "Scene/SceneSerializer.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <exception>
#include <fstream>
#include <limits>
#include <system_error>
#include <unordered_set>

namespace PulseForge
{
	namespace
	{
		using Json = nlohmann::ordered_json;
		constexpr uint64_t MinimumSupportedPrefabFormatVersion = 1;
		constexpr uint64_t PrefabFormatVersion = 3;
		constexpr std::string_view PrefabFormatName = "PulseForgePrefab";
		constexpr size_t MaxPrefabDocumentBytes = 64u * 1024u * 1024u;

		PrefabError MakeError(PrefabErrorCode Code, std::string Message)
		{
			return { Code, std::move(Message) };
		}

		PrefabError SceneOperationError(const SceneError& Error)
		{
			return MakeError(PrefabErrorCode::SceneOperationFailed, Error.Message);
		}

		std::expected<void, PrefabError> CopyEntityComponents(
			const Entity& Source,
			const Entity& Destination,
			bool ClearPrimaryRoles)
		{
			const auto Transform = Source.GetTransform();
			const auto Camera = Source.GetCamera();
			const auto DirectionalLight = Source.GetDirectionalLight();
			const auto EnvironmentLight = Source.GetEnvironmentLight();
			const auto PointLight = Source.GetPointLight();
			const auto SpotLight = Source.GetSpotLight();
			const auto MeshRenderer = Source.GetMeshRenderer();
			const auto Rigidbody = Source.GetRigidbody();
			const auto BoxCollider = Source.GetBoxCollider();
			const auto AudioSource = Source.GetAudioSource();
			const auto AudioListener = Source.GetAudioListener();
			const auto Script = Source.GetScript();
			if (!Transform || !Camera || !DirectionalLight || !EnvironmentLight || !PointLight || !SpotLight || !MeshRenderer || !Rigidbody || !BoxCollider ||
				!AudioSource || !AudioListener || !Script)
				return std::unexpected(MakeError(
					PrefabErrorCode::SceneOperationFailed,
					"Could not read supported entity components for prefab operation"));

			if (auto Result = Destination.SetTransform(*Transform); !Result)
				return std::unexpected(SceneOperationError(Result.error()));
			if (Camera->has_value())
			{
				CameraComponent InstanceCamera = Camera->value();
				if (ClearPrimaryRoles) InstanceCamera.IsPrimary = false;
				if (auto Result = Destination.SetCamera(InstanceCamera); !Result)
					return std::unexpected(SceneOperationError(Result.error()));
			}
			if (DirectionalLight->has_value())
				if (auto Result = Destination.SetDirectionalLight(DirectionalLight->value()); !Result)
					return std::unexpected(SceneOperationError(Result.error()));
			if (EnvironmentLight->has_value())
				if (auto Result = Destination.SetEnvironmentLight(EnvironmentLight->value()); !Result)
					return std::unexpected(SceneOperationError(Result.error()));
			if (PointLight->has_value())
				if (auto Result = Destination.SetPointLight(PointLight->value()); !Result)
					return std::unexpected(SceneOperationError(Result.error()));
			if (SpotLight->has_value())
				if (auto Result = Destination.SetSpotLight(SpotLight->value()); !Result)
					return std::unexpected(SceneOperationError(Result.error()));
			if (MeshRenderer->has_value())
			{
				if (auto Result = Destination.SetMeshRenderer(MeshRenderer->value()); !Result)
					return std::unexpected(SceneOperationError(Result.error()));
			}
			if (Rigidbody->has_value())
			{
				if (auto Result = Destination.SetRigidbody(Rigidbody->value()); !Result)
					return std::unexpected(SceneOperationError(Result.error()));
			}
			if (BoxCollider->has_value())
			{
				if (auto Result = Destination.SetBoxCollider(BoxCollider->value()); !Result)
					return std::unexpected(SceneOperationError(Result.error()));
			}
			if (AudioSource->has_value())
			{
				if (auto Result = Destination.SetAudioSource(AudioSource->value()); !Result)
					return std::unexpected(SceneOperationError(Result.error()));
			}
			if (AudioListener->has_value())
			{
				AudioListenerComponent InstanceListener = AudioListener->value();
				if (ClearPrimaryRoles) InstanceListener.IsPrimary = false;
				if (auto Result = Destination.SetAudioListener(InstanceListener); !Result)
					return std::unexpected(SceneOperationError(Result.error()));
			}
			if (Script->has_value())
			{
				if (auto Result = Destination.SetScript(Script->value()); !Result)
					return std::unexpected(SceneOperationError(Result.error()));
			}
			return {};
		}

		void RollbackEntities(Scene& Destination, const std::vector<Entity>& Entities) noexcept
		{
			for (auto Iterator = Entities.rbegin(); Iterator != Entities.rend(); ++Iterator)
				(void)Destination.DestroyEntity(*Iterator);
		}

		struct TemporaryFileCleanup
		{
			std::filesystem::path Path;

			~TemporaryFileCleanup()
			{
				std::error_code Error;
				std::filesystem::remove(Path, Error);
			}
		};
	}

	PrefabSourceDocument::PrefabSourceDocument(
		std::unique_ptr<Scene> SourceScene,
		UUID RootKey,
		AssetID SourceAsset,
		std::string Fingerprint,
		std::string OriginalData)
		: m_Scene(std::move(SourceScene)), m_RootKey(RootKey), m_SourceAsset(SourceAsset),
		  m_Fingerprint(std::move(Fingerprint)), m_OriginalData(std::move(OriginalData))
	{
	}

	std::expected<std::string, PrefabError> PrefabSerializer::Serialize(const Scene& Source, const Entity& Root)
	{
		const auto OwnedRoot = Source.FindEntity(Root.GetUUID());
		if (!Root.IsValid() || !OwnedRoot || *OwnedRoot != Root)
			return std::unexpected(MakeError(
				PrefabErrorCode::InvalidRootEntity,
				"Prefab root must be a valid entity owned by the source scene"));

		try
		{
			std::vector<Entity> SourceEntities{ Root };
			std::unordered_set<UUID, UUIDHash> IncludedEntities;
			IncludedEntities.reserve(Source.GetEntityCount());
			IncludedEntities.insert(Root.GetUUID());
			for (size_t Index = 0; Index < SourceEntities.size(); ++Index)
			{
				const auto Children = SourceEntities[Index].GetChildren();
				if (!Children)
					return std::unexpected(SceneOperationError(Children.error()));

				for (const Entity& Child : *Children)
				{
					if (!IncludedEntities.insert(Child.GetUUID()).second)
						return std::unexpected(MakeError(
							PrefabErrorCode::SceneOperationFailed,
							"Source hierarchy contains a cycle or duplicate child relationship"));
					SourceEntities.push_back(Child);
				}
			}

			for (const Entity& SourceEntity : SourceEntities)
			{
				const auto LinkedInstance = SourceEntity.GetPrefabInstance();
				if (!LinkedInstance)
					return std::unexpected(SceneOperationError(LinkedInstance.error()));
				if (LinkedInstance->has_value())
					return std::unexpected(MakeError(
						PrefabErrorCode::InvalidRootEntity,
						"Nested linked prefab instances are not supported in prefab source data"));
			}

			Scene PrefabScene;
			for (const Entity& SourceEntity : SourceEntities)
			{
				const auto Tag = SourceEntity.GetTag();
				if (!Tag)
					return std::unexpected(SceneOperationError(Tag.error()));
				auto Created = PrefabScene.CreateEntityWithUUID(SourceEntity.GetUUID(), Tag->Name);
				if (!Created)
					return std::unexpected(SceneOperationError(Created.error()));
				if (auto CopyResult = CopyEntityComponents(SourceEntity, *Created, false); !CopyResult)
					return std::unexpected(CopyResult.error());
			}

			for (const Entity& SourceEntity : SourceEntities)
			{
				const auto Parent = SourceEntity.GetParent();
				if (!Parent)
					return std::unexpected(SceneOperationError(Parent.error()));
				if (!Parent->has_value() || !IncludedEntities.contains((**Parent).GetUUID()))
					continue;

				const auto ChildCopy = PrefabScene.FindEntity(SourceEntity.GetUUID());
				const auto ParentCopy = PrefabScene.FindEntity((**Parent).GetUUID());
				if (!ChildCopy || !ParentCopy)
					return std::unexpected(MakeError(
						PrefabErrorCode::SceneOperationFailed,
						"Could not reconstruct prefab hierarchy in the staging scene"));
				if (auto ParentResult = ChildCopy->SetParent(*ParentCopy); !ParentResult)
					return std::unexpected(SceneOperationError(ParentResult.error()));
			}

			const auto SceneData = SceneSerializer::Serialize(PrefabScene);
			if (!SceneData)
				return std::unexpected(MakeError(PrefabErrorCode::SceneSerializationFailed, SceneData.error().Message));

			Json Document = Json::object();
			Document["format"] = PrefabFormatName;
			Document["version"] = PrefabFormatVersion;
			Document["root"] = Root.GetUUID().ToString();
			Document["scene"] = Json::parse(*SceneData);
			return Document.dump(2) + "\n";
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(MakeError(
				PrefabErrorCode::SceneSerializationFailed,
				std::string("Prefab serialization failed: ") + Exception.what()));
		}
	}

	std::expected<std::string, PrefabError> PrefabSerializer::Serialize(const PrefabSourceDocument& Source)
	{
		const auto Root = Source.GetScene().FindEntity(Source.GetRootKey());
		if (!Root)
			return std::unexpected(MakeError(PrefabErrorCode::InvalidRootEntity, "Prefab source document root no longer exists"));
		std::unordered_set<UUID, UUIDHash> Included{ Root->GetUUID() };
		std::vector<Entity> Pending{ *Root };
		for (size_t Index = 0; Index < Pending.size(); ++Index)
		{
			auto Children = Pending[Index].GetChildren();
			if (!Children) return std::unexpected(SceneOperationError(Children.error()));
			for (const Entity& Child : *Children)
			{
				if (!Included.insert(Child.GetUUID()).second)
					return std::unexpected(MakeError(PrefabErrorCode::InvalidRootEntity,
						"Prefab source hierarchy contains a cycle or duplicate child"));
				Pending.push_back(Child);
			}
		}
		if (Included.size() != Source.GetScene().GetEntityCount())
			return std::unexpected(MakeError(PrefabErrorCode::InvalidRootEntity,
				"Every entity in a prefab source document must remain under its stable root; detached entities would not be saved"));
		return Serialize(Source.GetScene(), *Root);
	}

	std::expected<Entity, PrefabError> PrefabSerializer::Instantiate(std::string_view Data, Scene& Destination)
	{
		std::vector<Entity> CreatedEntities;
		const auto Rollback = [&Destination, &CreatedEntities]() noexcept
		{
			RollbackEntities(Destination, CreatedEntities);
		};

		try
		{
			const Json Document = Json::parse(Data.begin(), Data.end());
			if (!Document.is_object())
				return std::unexpected(MakeError(PrefabErrorCode::InvalidDocument, "Prefab document root must be an object"));

			const auto Format = Document.find("format");
			if (Format == Document.end() || !Format->is_string())
				return std::unexpected(MakeError(PrefabErrorCode::InvalidDocument, "Prefab document is missing its format identifier"));
			if (Format->get<std::string>() != PrefabFormatName)
				return std::unexpected(MakeError(PrefabErrorCode::UnsupportedFormat, "Document is not a PulseForge prefab"));

			const auto Version = Document.find("version");
			if (Version == Document.end() || (!Version->is_number_integer() && !Version->is_number_unsigned()))
				return std::unexpected(MakeError(PrefabErrorCode::InvalidDocument, "Prefab document is missing a numeric version"));
			const bool SupportedVersion = Version->is_number_unsigned()
				? Version->get<uint64_t>() >= MinimumSupportedPrefabFormatVersion &&
					Version->get<uint64_t>() <= PrefabFormatVersion
				: Version->get<int64_t>() >= static_cast<int64_t>(MinimumSupportedPrefabFormatVersion) &&
					Version->get<int64_t>() <= static_cast<int64_t>(PrefabFormatVersion);
			if (!SupportedVersion)
				return std::unexpected(MakeError(PrefabErrorCode::UnsupportedVersion, "Prefab document version is not supported"));

			const auto RootValue = Document.find("root");
			const auto SceneValue = Document.find("scene");
			if (RootValue == Document.end() || !RootValue->is_string() || SceneValue == Document.end() || !SceneValue->is_object())
				return std::unexpected(MakeError(
					PrefabErrorCode::InvalidDocument,
					"Prefab requires a root entity UUID and embedded scene object"));

			const auto RootIdentifier = UUID::Parse(RootValue->get<std::string>());
			if (!RootIdentifier || RootIdentifier->IsNil())
				return std::unexpected(MakeError(
					PrefabErrorCode::InvalidDocument,
					RootIdentifier ? "Prefab root UUID must not be nil" : RootIdentifier.error().Message));

			Scene PrefabScene;
			const std::string SceneData = SceneValue->dump();
			if (auto LoadResult = SceneSerializer::Deserialize(SceneData, PrefabScene); !LoadResult)
				return std::unexpected(MakeError(
					PrefabErrorCode::SceneSerializationFailed,
					LoadResult.error().Message));

			const auto PrefabRoot = PrefabScene.FindEntity(*RootIdentifier);
			if (!PrefabRoot)
				return std::unexpected(MakeError(PrefabErrorCode::InvalidDocument, "Prefab root UUID is not present in its scene data"));
			const auto RootParent = PrefabRoot->GetParent();
			if (!RootParent || RootParent->has_value())
				return std::unexpected(MakeError(PrefabErrorCode::InvalidDocument, "Prefab root must not have a parent inside its scene data"));

			std::unordered_set<UUID, UUIDHash> ReachableEntities;
			std::vector<Entity> PrefabEntities{ *PrefabRoot };
			ReachableEntities.insert(*RootIdentifier);
			for (size_t Index = 0; Index < PrefabEntities.size(); ++Index)
			{
				const auto Children = PrefabEntities[Index].GetChildren();
				if (!Children)
					return std::unexpected(SceneOperationError(Children.error()));
				for (const Entity& Child : *Children)
				{
					if (!ReachableEntities.insert(Child.GetUUID()).second)
						return std::unexpected(MakeError(PrefabErrorCode::InvalidDocument, "Prefab hierarchy contains a repeated entity"));
					PrefabEntities.push_back(Child);
				}
			}
			if (ReachableEntities.size() != PrefabScene.GetEntityCount())
				return std::unexpected(MakeError(PrefabErrorCode::InvalidDocument, "Prefab scene contains entities outside the root hierarchy"));
			for (const Entity& PrefabEntity : PrefabEntities)
			{
				const auto LinkedInstance = PrefabEntity.GetPrefabInstance();
				if (!LinkedInstance)
				{
					Rollback();
					return std::unexpected(SceneOperationError(LinkedInstance.error()));
				}
				if (LinkedInstance->has_value())
				{
					Rollback();
					return std::unexpected(MakeError(
						PrefabErrorCode::InvalidDocument,
						"Nested linked prefab instances are not supported in prefab source data"));
				}
			}

			CreatedEntities.reserve(PrefabEntities.size());
			std::unordered_map<UUID, Entity, UUIDHash> InstantiatedEntities;
			InstantiatedEntities.reserve(PrefabEntities.size());
			for (const Entity& PrefabEntity : PrefabEntities)
			{
				const auto Tag = PrefabEntity.GetTag();
				if (!Tag)
				{
					Rollback();
					return std::unexpected(SceneOperationError(Tag.error()));
				}
				auto Created = Destination.CreateEntity(Tag->Name);
				if (!Created)
				{
					Rollback();
					return std::unexpected(SceneOperationError(Created.error()));
				}
				CreatedEntities.push_back(*Created);
				InstantiatedEntities.emplace(PrefabEntity.GetUUID(), *Created);
				if (auto CopyResult = CopyEntityComponents(PrefabEntity, *Created, true); !CopyResult)
				{
					Rollback();
					return std::unexpected(CopyResult.error());
				}
			}

			for (const Entity& PrefabEntity : PrefabEntities)
			{
				const auto Parent = PrefabEntity.GetParent();
				if (!Parent)
				{
					Rollback();
					return std::unexpected(SceneOperationError(Parent.error()));
				}
				if (!Parent->has_value())
					continue;

				const auto Child = InstantiatedEntities.find(PrefabEntity.GetUUID());
				const auto ParentCopy = InstantiatedEntities.find((**Parent).GetUUID());
				if (Child == InstantiatedEntities.end() || ParentCopy == InstantiatedEntities.end())
				{
					Rollback();
					return std::unexpected(MakeError(
						PrefabErrorCode::InvalidDocument,
						"Prefab hierarchy references an entity outside its root subtree"));
				}
				if (auto ParentResult = Child->second.SetParent(ParentCopy->second); !ParentResult)
				{
					Rollback();
					return std::unexpected(SceneOperationError(ParentResult.error()));
				}
			}

			return InstantiatedEntities.at(*RootIdentifier);
		}
		catch (const nlohmann::json::exception& Exception)
		{
			Rollback();
			return std::unexpected(MakeError(
				PrefabErrorCode::InvalidDocument,
				std::string("Prefab JSON is invalid: ") + Exception.what()));
		}
		catch (const std::exception& Exception)
		{
			Rollback();
			return std::unexpected(MakeError(
				PrefabErrorCode::InstantiationFailed,
				std::string("Prefab instantiation failed: ") + Exception.what()));
		}
	}

	std::expected<PrefabSourceDocument, PrefabError> PrefabSerializer::Parse(
		std::string_view Data,
		AssetID SourceAsset,
		std::string Fingerprint)
	{
		try
		{
			const Json Document = Json::parse(Data.begin(), Data.end());
			if (!Document.is_object())
				return std::unexpected(MakeError(PrefabErrorCode::InvalidDocument, "Prefab document root must be an object"));

			const auto Format = Document.find("format");
			if (Format == Document.end() || !Format->is_string())
				return std::unexpected(MakeError(PrefabErrorCode::InvalidDocument, "Prefab document is missing its format identifier"));
			if (Format->get<std::string>() != PrefabFormatName)
				return std::unexpected(MakeError(PrefabErrorCode::UnsupportedFormat, "Document is not a PulseForge prefab"));

			const auto Version = Document.find("version");
			if (Version == Document.end() || (!Version->is_number_integer() && !Version->is_number_unsigned()))
				return std::unexpected(MakeError(PrefabErrorCode::InvalidDocument, "Prefab document is missing a numeric version"));
			const bool SupportedVersion = Version->is_number_unsigned()
				? Version->get<uint64_t>() >= MinimumSupportedPrefabFormatVersion && Version->get<uint64_t>() <= PrefabFormatVersion
				: Version->get<int64_t>() >= static_cast<int64_t>(MinimumSupportedPrefabFormatVersion) &&
					Version->get<int64_t>() <= static_cast<int64_t>(PrefabFormatVersion);
			if (!SupportedVersion)
				return std::unexpected(MakeError(PrefabErrorCode::UnsupportedVersion, "Prefab document version is not supported"));

			const auto RootValue = Document.find("root");
			const auto SceneValue = Document.find("scene");
			if (RootValue == Document.end() || !RootValue->is_string() || SceneValue == Document.end() || !SceneValue->is_object())
				return std::unexpected(MakeError(
					PrefabErrorCode::InvalidDocument,
					"Prefab requires a root entity UUID and embedded scene object"));

			const auto RootKey = UUID::Parse(RootValue->get<std::string>());
			if (!RootKey || RootKey->IsNil())
				return std::unexpected(MakeError(
					PrefabErrorCode::InvalidDocument,
					RootKey ? "Prefab root UUID must not be nil" : RootKey.error().Message));

			auto SourceScene = std::make_unique<Scene>();
			const std::string SceneData = SceneValue->dump();
			if (auto LoadResult = SceneSerializer::Deserialize(SceneData, *SourceScene); !LoadResult)
				return std::unexpected(MakeError(PrefabErrorCode::SceneSerializationFailed, LoadResult.error().Message));

			const auto Root = SourceScene->FindEntity(*RootKey);
			if (!Root)
				return std::unexpected(MakeError(PrefabErrorCode::InvalidDocument, "Prefab root UUID is not present in its scene data"));
			const auto RootParent = Root->GetParent();
			if (!RootParent || RootParent->has_value())
				return std::unexpected(MakeError(PrefabErrorCode::InvalidDocument, "Prefab root must not have a parent inside its scene data"));

			std::unordered_set<UUID, UUIDHash> ReachableEntities{ *RootKey };
			std::vector<Entity> SourceEntities{ *Root };
			for (size_t Index = 0; Index < SourceEntities.size(); ++Index)
			{
				const auto Children = SourceEntities[Index].GetChildren();
				if (!Children) return std::unexpected(SceneOperationError(Children.error()));
				for (const Entity& Child : *Children)
				{
					if (!ReachableEntities.insert(Child.GetUUID()).second)
						return std::unexpected(MakeError(PrefabErrorCode::InvalidDocument, "Prefab hierarchy contains a repeated entity"));
					SourceEntities.push_back(Child);
				}
			}
			if (ReachableEntities.size() != SourceScene->GetEntityCount())
				return std::unexpected(MakeError(PrefabErrorCode::InvalidDocument, "Prefab scene contains entities outside the root hierarchy"));
			for (const Entity& Current : SourceEntities)
			{
				const auto Linked = Current.GetPrefabInstance();
				if (!Linked) return std::unexpected(SceneOperationError(Linked.error()));
				if (Linked->has_value())
					return std::unexpected(MakeError(
						PrefabErrorCode::InvalidDocument,
						"Nested linked prefab instances are not supported in prefab source data"));
			}

			return PrefabSourceDocument(
				std::move(SourceScene), *RootKey, SourceAsset, std::move(Fingerprint), std::string(Data));
		}
		catch (const nlohmann::json::exception& Exception)
		{
			return std::unexpected(MakeError(PrefabErrorCode::InvalidDocument, std::string("Prefab JSON is invalid: ") + Exception.what()));
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(MakeError(PrefabErrorCode::InvalidDocument, std::string("Prefab parsing failed: ") + Exception.what()));
		}
	}

	std::expected<PrefabInstantiationResult, PrefabError> PrefabSerializer::Instantiate(
		const PrefabSourceDocument& Source,
		Scene& Destination,
		bool ClearPrimaryRoles)
	{
		const auto Root = Source.GetScene().FindEntity(Source.GetRootKey());
		if (!Root)
			return std::unexpected(MakeError(PrefabErrorCode::InvalidRootEntity, "Prefab source document root no longer exists"));

		std::vector<Entity> SourceEntities{ *Root };
		std::unordered_set<UUID, UUIDHash> IncludedEntities{ Root->GetUUID() };
		for (size_t Index = 0; Index < SourceEntities.size(); ++Index)
		{
			const auto Children = SourceEntities[Index].GetChildren();
			if (!Children) return std::unexpected(SceneOperationError(Children.error()));
			for (const Entity& Child : *Children)
			{
				if (!IncludedEntities.insert(Child.GetUUID()).second)
					return std::unexpected(MakeError(PrefabErrorCode::InvalidDocument, "Prefab hierarchy contains a repeated entity"));
				SourceEntities.push_back(Child);
			}
		}
		if (IncludedEntities.size() != Source.GetScene().GetEntityCount())
			return std::unexpected(MakeError(PrefabErrorCode::InvalidDocument, "Prefab source contains entities outside its root hierarchy"));
		for (const Entity& SourceEntity : SourceEntities)
		{
			const auto Linked = SourceEntity.GetPrefabInstance();
			if (!Linked) return std::unexpected(SceneOperationError(Linked.error()));
			if (Linked->has_value())
				return std::unexpected(MakeError(
					PrefabErrorCode::InvalidDocument,
					"Nested linked prefab instances are not supported in prefab source data"));
		}

		std::vector<Entity> CreatedEntities;
		const auto Rollback = [&Destination, &CreatedEntities]() noexcept { RollbackEntities(Destination, CreatedEntities); };
		try
		{
			CreatedEntities.reserve(SourceEntities.size());
			std::unordered_map<UUID, Entity, UUIDHash> Instantiated;
			Instantiated.reserve(SourceEntities.size());
			PrefabInstantiationResult Result;
			Result.SourceEntities.reserve(SourceEntities.size());
			for (const Entity& SourceEntity : SourceEntities)
			{
				const auto Tag = SourceEntity.GetTag();
				if (!Tag) { Rollback(); return std::unexpected(SceneOperationError(Tag.error())); }
				auto Created = Destination.CreateEntity(Tag->Name);
				if (!Created) { Rollback(); return std::unexpected(SceneOperationError(Created.error())); }
				CreatedEntities.push_back(*Created);
				Instantiated.emplace(SourceEntity.GetUUID(), *Created);
				Result.SourceEntities.push_back({ SourceEntity.GetUUID(), Created->GetUUID() });
				if (auto Copy = CopyEntityComponents(SourceEntity, *Created, ClearPrimaryRoles); !Copy)
				{
					Rollback();
					return std::unexpected(Copy.error());
				}
			}
			for (const Entity& SourceEntity : SourceEntities)
			{
				const auto Parent = SourceEntity.GetParent();
				if (!Parent) { Rollback(); return std::unexpected(SceneOperationError(Parent.error())); }
				if (!Parent->has_value()) continue;
				const auto ChildCopy = Instantiated.find(SourceEntity.GetUUID());
				const auto ParentCopy = Instantiated.find((**Parent).GetUUID());
				if (ChildCopy == Instantiated.end() || ParentCopy == Instantiated.end())
				{
					Rollback();
					return std::unexpected(MakeError(PrefabErrorCode::InvalidDocument, "Prefab hierarchy references an entity outside its root subtree"));
				}
				if (auto ParentResult = ChildCopy->second.SetParent(ParentCopy->second); !ParentResult)
				{
					Rollback();
					return std::unexpected(SceneOperationError(ParentResult.error()));
				}
			}
			Result.Root = Instantiated.at(Source.GetRootKey());
			return Result;
		}
		catch (const std::exception& Exception)
		{
			Rollback();
			return std::unexpected(MakeError(PrefabErrorCode::InstantiationFailed, std::string("Prefab instantiation failed: ") + Exception.what()));
		}
	}

	std::expected<void, PrefabError> PrefabSerializer::SaveToFile(
		const Scene& Source,
		const Entity& Root,
		const std::filesystem::path& Path)
	{
		if (Path.empty())
			return std::unexpected(MakeError(PrefabErrorCode::FileOpenFailed, "Prefab file path must not be empty"));

		const auto Serialized = Serialize(Source, Root);
		if (!Serialized)
			return std::unexpected(Serialized.error());
		if (Serialized->size() > MaxPrefabDocumentBytes ||
			Serialized->size() > static_cast<size_t>(std::numeric_limits<std::streamsize>::max()))
		{
			return std::unexpected(MakeError(
				PrefabErrorCode::FileWriteFailed,
				"Serialized prefab exceeds the supported document size"));
		}

		try
		{
			const auto TemporaryIdentifier = UUID::Generate();
			if (!TemporaryIdentifier)
				return std::unexpected(MakeError(PrefabErrorCode::FileWriteFailed, TemporaryIdentifier.error().Message));

			std::filesystem::path TemporaryPath = Path;
			TemporaryPath += "." + TemporaryIdentifier->ToString() + ".tmp";
			TemporaryFileCleanup Cleanup{ TemporaryPath };

			std::ofstream Output(TemporaryPath, std::ios::binary | std::ios::trunc);
			if (!Output.is_open())
				return std::unexpected(MakeError(PrefabErrorCode::FileOpenFailed, "Could not open temporary prefab file for writing"));

			Output.write(Serialized->data(), static_cast<std::streamsize>(Serialized->size()));
			Output.flush();
			if (!Output)
				return std::unexpected(MakeError(PrefabErrorCode::FileWriteFailed, "Could not write the complete prefab document"));
			Output.close();
			if (Output.fail())
				return std::unexpected(MakeError(PrefabErrorCode::FileWriteFailed, "Could not finish writing the prefab document"));

			std::error_code ReplaceError;
			std::filesystem::rename(TemporaryPath, Path, ReplaceError);
			if (ReplaceError)
			{
				return std::unexpected(MakeError(
					PrefabErrorCode::FileReplaceFailed,
					"Could not replace prefab file: " + ReplaceError.message()));
			}
			return {};
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(MakeError(
				PrefabErrorCode::FileWriteFailed,
				std::string("Prefab file save failed: ") + Exception.what()));
		}
	}

	std::expected<Entity, PrefabError> PrefabSerializer::InstantiateFromFile(
		const std::filesystem::path& Path,
		Scene& Destination)
	{
		if (Path.empty())
			return std::unexpected(MakeError(PrefabErrorCode::FileOpenFailed, "Prefab file path must not be empty"));

		try
		{
			std::ifstream Input(Path, std::ios::binary);
			if (!Input.is_open())
				return std::unexpected(MakeError(PrefabErrorCode::FileOpenFailed, "Could not open prefab file for reading"));

			std::string Data;
			Data.reserve(4096);
			std::array<char, 8192> Buffer{};
			while (Input)
			{
				Input.read(Buffer.data(), static_cast<std::streamsize>(Buffer.size()));
				const std::streamsize ReadCount = Input.gcount();
				if (ReadCount > 0)
				{
					const size_t Count = static_cast<size_t>(ReadCount);
					if (Count > MaxPrefabDocumentBytes - Data.size())
						return std::unexpected(MakeError(
							PrefabErrorCode::FileReadFailed,
							"Prefab document exceeds the supported 64 MiB size limit"));
					Data.append(Buffer.data(), Count);
				}
			}
			if (!Input.eof())
				return std::unexpected(MakeError(PrefabErrorCode::FileReadFailed, "Could not read the complete prefab document"));

			return Instantiate(Data, Destination);
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(MakeError(
				PrefabErrorCode::FileReadFailed,
				std::string("Prefab file load failed: ") + Exception.what()));
		}
	}
}
