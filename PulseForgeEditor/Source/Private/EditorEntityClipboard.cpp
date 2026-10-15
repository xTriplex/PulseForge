#include "Editor/EditorEntityClipboard.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <exception>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace PulseForgeEditor
{
	namespace
	{
		using EditorJson = nlohmann::ordered_json;

		std::expected<PulseForge::UUID, std::string> GenerateUUID()
		{
			auto Generated = PulseForge::UUID::Generate();
			if (!Generated) return std::unexpected(Generated.error().Message);
			return *Generated;
		}
	} // namespace

	bool EditorEntityClipboard::CanPaste(const PulseForge::UUID& Project) const noexcept
	{
		return HasData() && m_Project == Project;
	}

	std::expected<EditorEntityClipboard, std::string> EditorEntityClipboard::Capture(PulseForge::UUID Project,
		std::optional<PulseForge::AssetID> Scene, std::span<const PulseForge::UUID> Roots,
		std::span<const PulseForge::UUID> IncludedEntities, std::string_view SerializedScene)
	{
		try
		{
			const EditorJson Document = EditorJson::parse(SerializedScene);
			const auto Entities = Document.find("entities");
			if (Entities == Document.end() || !Entities->is_array())
				return std::unexpected("The serialized scene does not contain an entity array.");

			std::unordered_set<std::string> IncludedIdentifiers;
			IncludedIdentifiers.reserve(IncludedEntities.size());
			for (const PulseForge::UUID& Identifier : IncludedEntities)
				IncludedIdentifiers.insert(Identifier.ToString());

			EditorEntityClipboard Clipboard;
			Clipboard.m_Project = Project;
			Clipboard.m_Scene = Scene;
			Clipboard.m_Roots.assign(Roots.begin(), Roots.end());
			Clipboard.m_Records.reserve(IncludedIdentifiers.size());
			for (const EditorJson& Record : *Entities)
			{
				const auto Identifier = Record.find("uuid");
				if (Identifier == Record.end() || !Identifier->is_string())
					return std::unexpected("A serialized entity has a missing or invalid UUID.");
				if (IncludedIdentifiers.contains(Identifier->get<std::string>())) Clipboard.m_Records.push_back(Record.dump());
			}

			if (Clipboard.m_Records.size() != IncludedIdentifiers.size())
				return std::unexpected("The serialized scene did not contain every selected hierarchy entity.");
			return Clipboard;
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(std::string("Could not parse the serialized scene: ") + Exception.what());
		}
	}

	std::expected<EditorEntityClipboard::PreparedPaste, std::string> EditorEntityClipboard::PreparePaste(PulseForge::UUID TargetProject,
		std::optional<PulseForge::AssetID> TargetScene, std::string_view SerializedTargetScene, const AssetAvailable& IsAssetAvailable,
		IdentifierGenerator GenerateIdentifier) const
	{
		if (!CanPaste(TargetProject))
			return std::unexpected("The clipboard belongs to another project; copy the entities again in this project.");

		if (!GenerateIdentifier) GenerateIdentifier = GenerateUUID;

		try
		{
			EditorJson Document = EditorJson::parse(SerializedTargetScene);
			auto Entities = Document.find("entities");
			if (Entities == Document.end() || !Entities->is_array())
				return std::unexpected("The active scene does not contain a valid entity array.");

			const bool SameScene = m_Scene == TargetScene;
			std::unordered_map<std::string, PulseForge::UUID> RemappedIdentifiers;
			std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash> ExistingIdentifiers;
			std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash> DestinationIdentifiers;
			std::unordered_set<std::string> ExistingNames;
			for (const EditorJson& Record : *Entities)
			{
				const auto IdentifierValue = Record.find("uuid");
				if (IdentifierValue == Record.end() || !IdentifierValue->is_string())
					return std::unexpected("The active scene contains an entity with a missing or invalid UUID.");
				const auto Identifier = PulseForge::UUID::Parse(IdentifierValue->get<std::string>());
				if (!Identifier) return std::unexpected("The active scene contains an entity with an invalid UUID.");
				ExistingIdentifiers.insert(*Identifier);
				DestinationIdentifiers.insert(*Identifier);
				const auto Tag = Record.find("tag");
				if (Tag != Record.end() && Tag->is_object())
					if (const auto Name = Tag->find("name"); Name != Tag->end() && Name->is_string())
						ExistingNames.insert(Name->get<std::string>());
			}

			std::vector<EditorJson> NewRecords;
			NewRecords.reserve(m_Records.size());
			for (const std::string& SerializedRecord : m_Records)
			{
				EditorJson Record = EditorJson::parse(SerializedRecord);
				const auto IdentifierValue = Record.find("uuid");
				if (IdentifierValue == Record.end() || !IdentifierValue->is_string())
					return std::unexpected("The clipboard contains an entity with a missing or invalid UUID.");
				const std::string OldIdentifier = IdentifierValue->get<std::string>();
				const auto ParsedOldIdentifier = PulseForge::UUID::Parse(OldIdentifier);
				if (!ParsedOldIdentifier || RemappedIdentifiers.contains(OldIdentifier))
					return std::unexpected("The clipboard contains a duplicate or invalid entity UUID.");

				std::optional<PulseForge::UUID> NewIdentifier;
				for (size_t Attempt = 0; Attempt < 64; ++Attempt)
				{
					auto Generated = GenerateIdentifier();
					if (!Generated) return std::unexpected(Generated.error());
					if (!Generated->IsNil() && !ExistingIdentifiers.contains(*Generated))
					{
						NewIdentifier = *Generated;
						break;
					}
				}
				if (!NewIdentifier) return std::unexpected("Could not generate a unique entity UUID for the paste operation.");
				ExistingIdentifiers.insert(*NewIdentifier);
				RemappedIdentifiers.emplace(OldIdentifier, *NewIdentifier);
				NewRecords.push_back(std::move(Record));
			}

			std::unordered_set<std::string> RootIdentifiers;
			for (const PulseForge::UUID& Root : m_Roots)
				RootIdentifiers.insert(Root.ToString());
			PreparedPaste Prepared;
			Prepared.EntityCount = NewRecords.size();
			for (EditorJson& Record : NewRecords)
			{
				const std::string OldIdentifier = Record.at("uuid").get<std::string>();
				const PulseForge::UUID NewIdentifier = RemappedIdentifiers.at(OldIdentifier);
				Record["uuid"] = NewIdentifier.ToString();

				const auto Tag = Record.find("tag");
				if (Tag != Record.end() && Tag->is_object() && Tag->contains("name") && Tag->at("name").is_string())
				{
					const std::string BaseName = Tag->at("name").get<std::string>() + " Copy";
					std::string Name = BaseName;
					for (uint32_t Suffix = 2; ExistingNames.contains(Name); ++Suffix)
						Name = BaseName + " " + std::to_string(Suffix);
					ExistingNames.insert(Name);
					(*Tag)["name"] = std::move(Name);
				}

				const auto Parent = Record.find("parent");
				if (Parent != Record.end() && Parent->is_string())
				{
					const std::string OldParent = Parent->get<std::string>();
					if (const auto InternalParent = RemappedIdentifiers.find(OldParent); InternalParent != RemappedIdentifiers.end())
						*Parent = InternalParent->second.ToString();
					else
					{
						const auto ParentIdentifier = PulseForge::UUID::Parse(OldParent);
						if (!SameScene || !ParentIdentifier || !DestinationIdentifiers.contains(*ParentIdentifier))
							return std::unexpected("This hierarchy has an external parent that is unavailable in the destination scene.");
					}
				}

				const auto CheckAsset = [&IsAssetAvailable](const EditorJson& Object, std::string_view Field) -> std::optional<std::string>
				{
					const auto Value = Object.find(Field);
					if (Value == Object.end()) return std::nullopt;
					if (!Value->is_string()) return "A copied asset reference is malformed.";
					const auto Identifier = PulseForge::UUID::Parse(Value->get<std::string>());
					if (!Identifier || !IsAssetAvailable || !IsAssetAvailable(*Identifier))
						return "A copied entity references an asset that is not available in this project.";
					return std::nullopt;
				};
				for (const auto& [ComponentName, Fields] : std::array{
						 std::pair<std::string_view, std::array<std::string_view, 2>>{ "meshRenderer", { "meshAsset", "materialAsset" } },
						 std::pair<std::string_view, std::array<std::string_view, 2>>{ "environmentLight", { "hdrImage", "" } },
						 std::pair<std::string_view, std::array<std::string_view, 2>>{ "audioSource", { "asset", "" } },
						 std::pair<std::string_view, std::array<std::string_view, 2>>{ "script", { "asset", "" } } })
				{
					const auto Component = Record.find(ComponentName);
					if (Component == Record.end() || !Component->is_object()) continue;
					for (const std::string_view Field : Fields)
						if (!Field.empty())
							if (const auto Error = CheckAsset(*Component, Field)) return std::unexpected(*Error);
				}

				if (RootIdentifiers.contains(OldIdentifier)) Prepared.RootIdentifiers.push_back(NewIdentifier);
				if (auto Camera = Record.find("camera"); Camera != Record.end() && Camera->is_object()) (*Camera)["primary"] = false;
				if (auto Listener = Record.find("audioListener"); Listener != Record.end() && Listener->is_object())
					(*Listener)["primary"] = false;
			}

			for (EditorJson& Record : NewRecords)
				Entities->push_back(std::move(Record));
			Prepared.SerializedScene = Document.dump(2) + "\n";
			return Prepared;
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(Exception.what());
		}
	}
} // namespace PulseForgeEditor
