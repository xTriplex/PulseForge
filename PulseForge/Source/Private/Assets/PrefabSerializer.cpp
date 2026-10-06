#include "Core/PulseForgePCH.h"
#include "Assets/PrefabSerializer.h"

#include "Scene/Components/MeshRendererComponent.h"
#include "Scene/SceneSerializer.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <exception>
#include <fstream>
#include <limits>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace PulseForge
{
	namespace
	{
		using Json = nlohmann::ordered_json;
		constexpr uint64_t MinimumSupportedPrefabFormatVersion = 1;
		constexpr uint64_t PrefabFormatVersion = 2;
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

		std::expected<void, PrefabError> CopyEntityComponents(const Entity& Source, const Entity& Destination)
		{
			const auto Transform = Source.GetTransform();
			const auto Camera = Source.GetCamera();
			const auto MeshRenderer = Source.GetMeshRenderer();
			if (!Transform || !Camera || !MeshRenderer)
				return std::unexpected(MakeError(
					PrefabErrorCode::SceneOperationFailed,
					"Could not read supported entity components for prefab operation"));

			if (auto Result = Destination.SetTransform(*Transform); !Result)
				return std::unexpected(SceneOperationError(Result.error()));
			if (Camera->has_value())
			{
				if (auto Result = Destination.SetCamera(Camera->value()); !Result)
					return std::unexpected(SceneOperationError(Result.error()));
			}
			if (MeshRenderer->has_value())
			{
				if (auto Result = Destination.SetMeshRenderer(MeshRenderer->value()); !Result)
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

			Scene PrefabScene;
			for (const Entity& SourceEntity : SourceEntities)
			{
				const auto Tag = SourceEntity.GetTag();
				if (!Tag)
					return std::unexpected(SceneOperationError(Tag.error()));
				auto Created = PrefabScene.CreateEntityWithUUID(SourceEntity.GetUUID(), Tag->Name);
				if (!Created)
					return std::unexpected(SceneOperationError(Created.error()));
				if (auto CopyResult = CopyEntityComponents(SourceEntity, *Created); !CopyResult)
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
				if (auto CopyResult = CopyEntityComponents(PrefabEntity, *Created); !CopyResult)
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
