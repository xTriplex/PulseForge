#include "Input/GameplayInputStore.h"

#include "Core/PlatformServices.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <system_error>
#include <unordered_set>

namespace PulseForge
{
	namespace
	{
		using Json = nlohmann::json;
		constexpr size_t MaximumSettingsBytes = 1024 * 1024;
		constexpr size_t MaximumOverrides = 1024;

		struct FileReadResult
		{
			std::string Contents;
			std::string Error;
		};

		FileReadResult ReadBounded(const std::filesystem::path& Path)
		{
			std::error_code Error;
			const uintmax_t Size = std::filesystem::file_size(Path, Error);
			if (Error) return { {}, "Could not inspect '" + Path.string() + "': " + Error.message() };
			if (Size > MaximumSettingsBytes) return { {}, "Input map settings exceed the 1 MiB size limit" };

			std::ifstream Stream(Path, std::ios::binary);
			if (!Stream) return { {}, "Could not open '" + Path.string() + "' for reading" };
			std::string Contents(static_cast<size_t>(Size), '\0');
			if (!Contents.empty() && !Stream.read(Contents.data(), static_cast<std::streamsize>(Contents.size())))
				return { {}, "Could not read all of '" + Path.string() + "'" };
			return { std::move(Contents), {} };
		}

		std::filesystem::path OverridesPath(const std::filesystem::path& UserDataRoot, const UUID& ProjectId)
		{
			return UserDataRoot / "PulseForge" / "InputOverrides" / (ProjectId.ToString() + ".json");
		}

		std::expected<InputControl, GameplayInputError> ReadControl(const Json& Value, std::string_view Location)
		{
			if (!Value.is_string())
				return std::unexpected(GameplayInputError{ std::string(Location) + " must name a supported input control" });
			auto Control = ParseInputControlName(Value.get_ref<const std::string&>());
			if (!Control)
				return std::unexpected(GameplayInputError{ std::string(Location) + " names an unsupported input control" });
			return *Control;
		}

		std::expected<Json, GameplayInputError> ReadOverrides(const std::filesystem::path& Path, const UUID& ProjectId)
		{
			std::error_code ExistsError;
			const bool Exists = std::filesystem::exists(Path, ExistsError);
			if (ExistsError)
				return std::unexpected(GameplayInputError{ "Could not inspect user input overrides: " + ExistsError.message() });
			if (!Exists)
				return Json{ { "version", 1 }, { "projectId", ProjectId.ToString() }, { "overrides", Json::array() } };

			FileReadResult Read = ReadBounded(Path);
			if (!Read.Error.empty()) return std::unexpected(GameplayInputError{ std::move(Read.Error) });
			Json Root = Json::parse(Read.Contents, nullptr, false);
			if (Root.is_discarded() || !Root.is_object())
				return std::unexpected(GameplayInputError{ "User input override settings contain malformed JSON" });
			if (!Root.contains("version") || !Root["version"].is_number_integer() || Root["version"].get<int64_t>() != 1)
				return std::unexpected(GameplayInputError{ "User input override settings use an unsupported version and were left untouched" });
			if (!Root.contains("projectId") || !Root["projectId"].is_string() ||
				Root["projectId"].get<std::string>() != ProjectId.ToString())
				return std::unexpected(GameplayInputError{ "User input override settings belong to a different project" });
			if (!Root.contains("overrides") || !Root["overrides"].is_array() || Root["overrides"].size() > MaximumOverrides)
				return std::unexpected(GameplayInputError{ "User input override collection is malformed or exceeds its limit" });
			return Root;
		}

		std::expected<void, GameplayInputError> WriteAtomically(
			const std::filesystem::path& Destination,
			std::string_view Contents)
		{
			std::error_code Error;
			std::filesystem::create_directories(Destination.parent_path(), Error);
			if (Error)
				return std::unexpected(GameplayInputError{ "Could not create input override directory: " + Error.message() });

			auto TemporaryId = UUID::Generate();
			if (!TemporaryId)
				return std::unexpected(GameplayInputError{ "Could not create a temporary input override identity: " + TemporaryId.error().Message });
			std::filesystem::path TemporaryPath = Destination;
			TemporaryPath += std::filesystem::path("." + TemporaryId->ToString() + ".tmp");
			{
				std::ofstream Stream(TemporaryPath, std::ios::binary | std::ios::trunc);
				if (!Stream)
					return std::unexpected(GameplayInputError{ "Could not create the temporary input override file" });
				Stream.write(Contents.data(), static_cast<std::streamsize>(Contents.size()));
				Stream.flush();
				if (!Stream)
				{
					Stream.close();
					std::filesystem::remove(TemporaryPath, Error);
					return std::unexpected(GameplayInputError{ "Could not write the complete input override file" });
				}
			}

			if (auto Replaced = Platform::ReplaceFileAtomically(TemporaryPath, Destination); !Replaced)
			{
				std::filesystem::remove(TemporaryPath, Error);
				return std::unexpected(GameplayInputError{ Replaced.error().Message });
			}
			return {};
		}

		bool IsStableIdentifier(std::string_view Value) noexcept
		{
			if (Value.empty() || Value.size() > 64) return false;
			const auto IsStart = [](unsigned char Character)
			{
				return (Character >= 'A' && Character <= 'Z') || (Character >= 'a' && Character <= 'z') || Character == '_';
			};
			if (!IsStart(static_cast<unsigned char>(Value.front()))) return false;
			for (char Character : Value)
			{
				const unsigned char Byte = static_cast<unsigned char>(Character);
				if (!IsStart(Byte) && !(Byte >= '0' && Byte <= '9') && Byte != '.' && Byte != '-') return false;
			}
			return true;
		}
	}

	std::expected<GameplayInputMapLoad, GameplayInputError> LoadGameplayInputMap(
		const std::filesystem::path& ProjectRoot,
		const UUID& ProjectId,
		const std::filesystem::path& UserDataRoot)
	{
		GameplayInputMapLoad Result;
		auto Defaults = LoadProjectGameplayActionMap(ProjectRoot);
		if (!Defaults) return std::unexpected(Defaults.error());
		Result.ActionMap = std::move(*Defaults);
		if (ProjectId.IsNil())
		{
			Result.Warnings.push_back("Project identity is unavailable; project-scoped input overrides were not loaded");
			return Result;
		}
		if (UserDataRoot.empty() || !UserDataRoot.is_absolute())
		{
			Result.Warnings.push_back("An absolute user-data directory is unavailable; input overrides were not loaded");
			return Result;
		}

		const std::filesystem::path Path = OverridesPath(UserDataRoot, ProjectId);
		std::error_code ExistsError;
		const bool HasOverrides = std::filesystem::exists(Path, ExistsError);
		if (ExistsError)
		{
			Result.Warnings.push_back("Could not inspect user input overrides: " + ExistsError.message());
			return Result;
		}
		if (!HasOverrides) return Result;

		auto Root = ReadOverrides(Path, ProjectId);
		if (!Root)
		{
			Result.Warnings.push_back(Root.error().Message);
			return Result;
		}

		GameplayActionMap Candidate = Result.ActionMap;
		std::unordered_set<std::string> SeenOverrideIds;
		for (size_t Index = 0; Index < (*Root)["overrides"].size(); ++Index)
		{
			const Json& Entry = (*Root)["overrides"][Index];
			const std::string Location = "overrides[" + std::to_string(Index) + "]";
			if (!Entry.is_object() || !Entry.contains("actionId") || !Entry["actionId"].is_string() ||
				!Entry.contains("bindingId") || !Entry["bindingId"].is_string() || !Entry.contains("primary"))
			{
				Result.Warnings.push_back(Location + " is malformed; user overrides were not applied");
				return Result;
			}
			const std::string ActionId = Entry["actionId"].get<std::string>();
			const std::string BindingId = Entry["bindingId"].get<std::string>();
			const std::string OverrideId = ActionId + "\n" + BindingId;
			if (!IsStableIdentifier(ActionId) || !IsStableIdentifier(BindingId) || !SeenOverrideIds.emplace(OverrideId).second)
			{
				Result.Warnings.push_back(Location + " has an invalid or duplicate action/binding identity; user overrides were not applied");
				return Result;
			}
			auto Action = std::find_if(Candidate.Actions.begin(), Candidate.Actions.end(),
				[&](const GameplayActionDefinition& Value) { return Value.Id == ActionId; });
			if (Action == Candidate.Actions.end()) continue;
			auto Binding = std::find_if(Action->Bindings.begin(), Action->Bindings.end(),
				[&](const ActionBinding& Value) { return Value.Id == BindingId; });
			if (Binding == Action->Bindings.end()) continue;

			auto Primary = ReadControl(Entry["primary"], Location + ".primary");
			if (!Primary)
			{
				Result.Warnings.push_back(Primary.error().Message + "; user overrides were not applied");
				return Result;
			}
			Binding->Primary = *Primary;
			if (Entry.contains("secondary"))
			{
				if (Entry["secondary"].is_null())
					Binding->Secondary = {};
				else
				{
					auto Secondary = ReadControl(Entry["secondary"], Location + ".secondary");
					if (!Secondary)
					{
						Result.Warnings.push_back(Secondary.error().Message + "; user overrides were not applied");
						return Result;
					}
					Binding->Secondary = *Secondary;
				}
			}
			if (Entry.contains("modifiers"))
			{
				if (!Entry["modifiers"].is_array() || Entry["modifiers"].size() > Binding->Modifiers.size())
				{
					Result.Warnings.push_back(Location + ".modifiers is malformed; user overrides were not applied");
					return Result;
				}
				Binding->ModifierCount = static_cast<uint8_t>(Entry["modifiers"].size());
				for (size_t ModifierIndex = 0; ModifierIndex < Entry["modifiers"].size(); ++ModifierIndex)
				{
					auto Modifier = ReadControl(Entry["modifiers"][ModifierIndex], Location + ".modifiers");
					if (!Modifier)
					{
						Result.Warnings.push_back(Modifier.error().Message + "; user overrides were not applied");
						return Result;
					}
					Binding->Modifiers[ModifierIndex] = *Modifier;
				}
			}
		}

		if (auto Valid = ValidateGameplayActionMap(Candidate); !Valid)
		{
			Result.Warnings.push_back("User input overrides were rejected: " + Valid.error().Message);
			return Result;
		}
		Result.ActionMap = std::move(Candidate);
		return Result;
	}

	std::expected<GameplayActionMap, GameplayInputError> LoadProjectGameplayActionMap(
		const std::filesystem::path& ProjectRoot)
	{
		const std::filesystem::path DefaultsPath = ProjectRoot / "InputActions.json";
		std::error_code Error;
		const bool Exists = std::filesystem::exists(DefaultsPath, Error);
		if (Error)
			return std::unexpected(GameplayInputError{ "Could not inspect project input defaults: " + Error.message() });
		if (!Exists) return GameplayActionMap{};

		FileReadResult Defaults = ReadBounded(DefaultsPath);
		if (!Defaults.Error.empty()) return std::unexpected(GameplayInputError{ std::move(Defaults.Error) });
		auto Parsed = ParseGameplayActionMap(Defaults.Contents);
		if (!Parsed)
			return std::unexpected(GameplayInputError{ "Could not load project input defaults: " + Parsed.error().Message });
		return Parsed;
	}

	std::expected<void, GameplayInputError> SaveGameplayInputBindingOverride(
		const std::filesystem::path& UserDataRoot,
		const UUID& ProjectId,
		std::string_view ActionId,
		std::string_view BindingId,
		const ActionBinding& Binding)
	{
		if (ProjectId.IsNil() || !IsStableIdentifier(ActionId) || !IsStableIdentifier(BindingId))
			return std::unexpected(GameplayInputError{ "Project, action, and binding identities must be valid" });
		const std::string_view PrimaryName = GetInputControlName(Binding.Primary);
		if (PrimaryName.empty() || !ParseInputControlName(PrimaryName))
			return std::unexpected(GameplayInputError{ "The primary input control is not supported for persistence" });
		const std::string_view SecondaryName = GetInputControlName(Binding.Secondary);
		if (!SecondaryName.empty() && !ParseInputControlName(SecondaryName))
			return std::unexpected(GameplayInputError{ "The secondary input control is not supported for persistence" });
		if (Binding.ModifierCount > Binding.Modifiers.size())
			return std::unexpected(GameplayInputError{ "The input binding has too many modifiers" });
		for (uint8_t Index = 0; Index < Binding.ModifierCount; ++Index)
			if (GetInputControlName(Binding.Modifiers[Index]).empty())
				return std::unexpected(GameplayInputError{ "The input binding contains an unsupported modifier" });
		if (UserDataRoot.empty() || !UserDataRoot.is_absolute())
			return std::unexpected(GameplayInputError{ "An absolute user-data directory is required to save input overrides" });

		const std::filesystem::path Path = OverridesPath(UserDataRoot, ProjectId);
		auto Root = ReadOverrides(Path, ProjectId);
		if (!Root) return std::unexpected(Root.error());
		Json* Matching = nullptr;
		for (Json& Entry : (*Root)["overrides"])
		{
			if (!Entry.is_object() || !Entry.contains("actionId") || !Entry["actionId"].is_string() ||
				!Entry.contains("bindingId") || !Entry["bindingId"].is_string())
				return std::unexpected(GameplayInputError{ "Existing user input overrides are malformed and were left untouched" });
			if (Entry["actionId"].get<std::string>() == ActionId &&
				Entry["bindingId"].get<std::string>() == BindingId)
			{
				if (Matching) return std::unexpected(GameplayInputError{ "Existing user input overrides contain a duplicate binding identity" });
				Matching = &Entry;
			}
		}
		if (!Matching)
		{
			if ((*Root)["overrides"].size() >= MaximumOverrides)
				return std::unexpected(GameplayInputError{ "User input overrides reached the 1024 entry limit" });
			(*Root)["overrides"].push_back(Json::object());
			Matching = &(*Root)["overrides"].back();
		}

		(*Matching)["actionId"] = ActionId;
		(*Matching)["bindingId"] = BindingId;
		(*Matching)["primary"] = GetInputControlName(Binding.Primary);
		(*Matching)["secondary"] = SecondaryName.empty() ? Json(nullptr) : Json(SecondaryName);
		Json Modifiers = Json::array();
		for (uint8_t Index = 0; Index < Binding.ModifierCount; ++Index)
			Modifiers.push_back(GetInputControlName(Binding.Modifiers[Index]));
		(*Matching)["modifiers"] = std::move(Modifiers);

		const std::string Contents = Root->dump(2) + "\n";
		if (Contents.size() > MaximumSettingsBytes)
			return std::unexpected(GameplayInputError{ "Serialized user input overrides exceed the 1 MiB size limit" });
		return WriteAtomically(Path, Contents);
	}
}
