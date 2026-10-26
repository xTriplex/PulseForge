#include "Assets/Detail/AssetEditRecoveryJournal.h"

#include "Core/PlatformServices.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#elif defined(__APPLE__) || defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace PulseForge::Assets::Detail
{
	namespace
	{
		using Json = nlohmann::ordered_json;
		using Path = std::filesystem::path;
		constexpr std::string_view JournalFormat = "PulseForge.AssetEditRecoveryJournal";
		constexpr std::string_view FingerprintAlgorithm = "xxh3-128-v0.8.2";
		constexpr std::string_view TransactionDirectoryPrefix = ".pf-asset-edit-";
		constexpr std::string_view JournalFilename = "RecoveryRecord.json";

		AssetEditRecoveryJournalError MakeError(AssetEditRecoveryJournalErrorCode Code, Path File, std::string Message)
		{
			return { Code, std::move(File), std::move(Message) };
		}

		std::string PathToUtf8(const Path& Value)
		{
			const std::u8string Encoded = Value.generic_u8string();
			return { reinterpret_cast<const char*>(Encoded.data()), Encoded.size() };
		}

		std::string LogicalPathKey(const Path& Value)
		{
			std::string Key = PathToUtf8(Value);
#if defined(_WIN32)
			std::transform(Key.begin(), Key.end(), Key.begin(), [](unsigned char Character)
				{ return Character >= 'A' && Character <= 'Z' ? static_cast<char>(Character - 'A' + 'a') : static_cast<char>(Character); });
#endif
			return Key;
		}

		std::string AsciiCaseFoldedPathSpellingKey(const Path& Value)
		{
			std::string Key = PathToUtf8(Value);
			std::transform(Key.begin(), Key.end(), Key.begin(), [](unsigned char Character)
				{ return Character >= 'A' && Character <= 'Z' ? static_cast<char>(Character - 'A' + 'a') : static_cast<char>(Character); });
			return Key;
		}

		Path PathFromUtf8(std::string_view Value)
		{
			return std::filesystem::u8path(Value.begin(), Value.end());
		}

		bool IsLowerHex(std::string_view Value)
		{
			return std::all_of(Value.begin(), Value.end(),
				[](unsigned char Character) { return (Character >= '0' && Character <= '9') || (Character >= 'a' && Character <= 'f'); });
		}

		bool IsValidUtf8(std::string_view Value)
		{
			for (size_t Offset = 0; Offset < Value.size();)
			{
				const auto First = static_cast<uint8_t>(Value[Offset]);
				if (First <= 0x7f)
				{
					++Offset;
					continue;
				}

				size_t Length = 0;
				uint32_t Codepoint = 0;
				if (First >= 0xc2 && First <= 0xdf)
				{
					Length = 2;
					Codepoint = First & 0x1f;
				}
				else if (First >= 0xe0 && First <= 0xef)
				{
					Length = 3;
					Codepoint = First & 0x0f;
				}
				else if (First >= 0xf0 && First <= 0xf4)
				{
					Length = 4;
					Codepoint = First & 0x07;
				}
				else return false;

				if (Length > Value.size() - Offset) return false;
				for (size_t Index = 1; Index < Length; ++Index)
				{
					const auto Next = static_cast<uint8_t>(Value[Offset + Index]);
					if ((Next & 0xc0) != 0x80) return false;
					Codepoint = (Codepoint << 6) | (Next & 0x3f);
				}
				if ((Length == 3 && Codepoint < 0x800) || (Length == 4 && Codepoint < 0x10000) ||
					(Codepoint >= 0xd800 && Codepoint <= 0xdfff) || Codepoint > 0x10ffff)
					return false;
				Offset += Length;
			}
			return true;
		}

		bool IsEmptyFingerprint(const AssetContentFingerprint& Fingerprint)
		{
			return Fingerprint.Algorithm.empty() && Fingerprint.DigestHex.empty();
		}

		bool IsValidFingerprint(const AssetContentFingerprint& Fingerprint)
		{
			return Fingerprint.Algorithm == FingerprintAlgorithm && Fingerprint.DigestHex.size() == 32 && IsLowerHex(Fingerprint.DigestHex);
		}

		bool IsLinkedOrReparsePoint(const Path& Value, const std::filesystem::file_status& Status)
		{
			if (std::filesystem::is_symlink(Status)) return true;
#if defined(_WIN32)
			const DWORD Attributes = GetFileAttributesW(Value.c_str());
			return Attributes != INVALID_FILE_ATTRIBUTES && (Attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
			(void)Value;
			return false;
#endif
		}

		enum class FinalPathKind : uint8_t
		{
			Any,
			File,
			Directory
		};

		std::expected<void, AssetEditRecoveryJournalError> ValidatePathAncestors(
			const Path& Value, bool AllowMissingTail, FinalPathKind FinalKind, bool RequireFinal)
		{
			if (Value.empty() || !Value.is_absolute() || Value.lexically_normal() != Value)
				return std::unexpected(MakeError(
					AssetEditRecoveryJournalErrorCode::InvalidJournal, Value, "Filesystem path must be absolute and lexically normalized"));

			Path Current = Value.root_path();
			bool MissingTail = false;
			std::error_code Error;
			const auto Inspect = [&](const Path& Candidate, bool IsFinal) -> std::expected<void, AssetEditRecoveryJournalError>
			{
				if (MissingTail) return {};
				Error.clear();
				const auto Status = std::filesystem::symlink_status(Candidate, Error);
				if (Error == std::errc::no_such_file_or_directory || (!Error && Status.type() == std::filesystem::file_type::not_found))
				{
					if (IsFinal && RequireFinal)
						return std::unexpected(
							MakeError(AssetEditRecoveryJournalErrorCode::ReadFailed, Candidate, "Required journal path does not exist"));
					if (!IsFinal && !AllowMissingTail)
						return std::unexpected(
							MakeError(AssetEditRecoveryJournalErrorCode::LinkedPath, Candidate, "A required path ancestor does not exist"));
					MissingTail = true;
					return {};
				}
				if (Error)
					return std::unexpected(MakeError(AssetEditRecoveryJournalErrorCode::LinkedPath, Candidate,
						"Could not inspect path or its ancestors: " + Error.message()));
				if (IsLinkedOrReparsePoint(Candidate, Status))
					return std::unexpected(MakeError(AssetEditRecoveryJournalErrorCode::LinkedPath, Candidate,
						"Linked or reparse paths are not allowed in a recovery journal"));
				if (IsFinal)
				{
					if (FinalKind == FinalPathKind::File && !std::filesystem::is_regular_file(Status))
						return std::unexpected(
							MakeError(AssetEditRecoveryJournalErrorCode::ReadFailed, Candidate, "Journal path is not a regular file"));
					if (FinalKind == FinalPathKind::Directory && !std::filesystem::is_directory(Status))
						return std::unexpected(
							MakeError(AssetEditRecoveryJournalErrorCode::InvalidJournal, Candidate, "Project root is not a directory"));
				}
				else if (!std::filesystem::is_directory(Status))
					return std::unexpected(
						MakeError(AssetEditRecoveryJournalErrorCode::LinkedPath, Candidate, "A path ancestor is not a directory"));
				return {};
			};

			if (auto Root = Inspect(Current, Current == Value); !Root) return Root;
			for (const Path& Part : Value.relative_path())
			{
				Current /= Part;
				if (auto Checked = Inspect(Current, Current == Value); !Checked) return Checked;
			}
			return {};
		}

		std::expected<Path, AssetEditRecoveryJournalError> ParseProjectRoot(std::string_view Utf8)
		{
			if (Utf8.empty() || Utf8.size() > AssetEditRecoveryJournal::MaximumPathUtf8Bytes || !IsValidUtf8(Utf8) ||
				Utf8.find('\0') != std::string_view::npos)
				return std::unexpected(MakeError(
					AssetEditRecoveryJournalErrorCode::InvalidJournal, {}, "Project-root association is empty or exceeds the path bound"));
			try
			{
				Path Root = PathFromUtf8(Utf8);
				if (!Root.is_absolute() || Root.lexically_normal() != Root || PathToUtf8(Root) != Utf8)
					return std::unexpected(MakeError(AssetEditRecoveryJournalErrorCode::InvalidJournal, Root,
						"Project-root association must be a normalized absolute generic UTF-8 path"));
				return Root;
			}
			catch (const std::exception& Exception)
			{
				return std::unexpected(MakeError(AssetEditRecoveryJournalErrorCode::InvalidJournal, {},
					"Project-root association is invalid UTF-8: " + std::string(Exception.what())));
			}
		}

		std::expected<void, AssetEditRecoveryJournalError> ValidateProjectRelativePath(
			const Path& Value, bool AllowEmpty, bool MustBeUnderAssets)
		{
			if (Value.empty())
			{
				if (AllowEmpty) return {};
				return std::unexpected(
					MakeError(AssetEditRecoveryJournalErrorCode::InvalidJournal, Value, "Required project-relative path is empty"));
			}
			const std::string Utf8 = PathToUtf8(Value);
			if (Utf8.size() > AssetEditRecoveryJournal::MaximumPathUtf8Bytes || !IsValidUtf8(Utf8) ||
				Utf8.find('\0') != std::string::npos || Value.is_absolute() || Value.has_root_name() || Value.has_root_directory() ||
				Value.lexically_normal() != Value || Utf8.find('\\') != std::string::npos || Utf8.find(':') != std::string::npos)
				return std::unexpected(MakeError(
					AssetEditRecoveryJournalErrorCode::InvalidJournal, Value, "Path is not a canonical project-relative UTF-8 path"));

			for (const Path& Part : Value)
				if (Part.empty() || Part == "." || Part == "..")
					return std::unexpected(MakeError(
						AssetEditRecoveryJournalErrorCode::InvalidJournal, Value, "Path contains an empty or traversal component"));
			if (MustBeUnderAssets)
			{
				auto Part = Value.begin();
				if (Part == Value.end() || *Part != "Assets" || ++Part == Value.end())
					return std::unexpected(
						MakeError(AssetEditRecoveryJournalErrorCode::InvalidJournal, Value, "Managed asset paths must be beneath Assets"));
			}
			return {};
		}

		std::expected<void, AssetEditRecoveryJournalError> ValidateTransactionRelativePath(const Path& Value)
		{
			const std::string Utf8 = PathToUtf8(Value);
			if (Value.empty() || Value.is_absolute() || Value.has_root_name() || Value.has_root_directory() ||
				Value.lexically_normal() != Value || Utf8.size() > AssetEditRecoveryJournal::MaximumPathUtf8Bytes || !IsValidUtf8(Utf8) ||
				Utf8.find('\0') != std::string::npos)
				return std::unexpected(MakeError(AssetEditRecoveryJournalErrorCode::InvalidJournal, Value,
					"Backup path must be a normalized transaction-relative path"));
			for (const Path& Part : Value)
				if (Part.empty() || Part == "." || Part == "..")
					return std::unexpected(MakeError(
						AssetEditRecoveryJournalErrorCode::InvalidJournal, Value, "Backup path contains an empty or traversal component"));
			return {};
		}

		std::expected<void, AssetEditRecoveryJournalError> ValidateEntryState(
			const AssetEditRecoveryEntryState& Entry, const Path& PathValue)
		{
			if (Entry.Type == AssetEditRecoveryEntryType::Directory)
			{
				if (Entry.ByteLength != 0 || !IsEmptyFingerprint(Entry.Content) || Entry.Identity)
					return std::unexpected(MakeError(AssetEditRecoveryJournalErrorCode::InvalidJournal, PathValue,
						"Directory states contain only their type and existence; content receipts and asset identities are invalid"));
				return {};
			}
			if (Entry.Type != AssetEditRecoveryEntryType::File)
				return std::unexpected(
					MakeError(AssetEditRecoveryJournalErrorCode::InvalidJournal, PathValue, "Unknown recovery entry type"));
			if (Entry.Exists)
			{
				if (!IsValidFingerprint(Entry.Content))
					return std::unexpected(MakeError(
						AssetEditRecoveryJournalErrorCode::InvalidJournal, PathValue, "File state has a malformed content fingerprint"));
				if (Entry.Identity && Entry.Identity->IsNil())
					return std::unexpected(
						MakeError(AssetEditRecoveryJournalErrorCode::InvalidJournal, PathValue, "File state has a nil asset identity"));
			}
			else if (Entry.ByteLength != 0 || !IsEmptyFingerprint(Entry.Content) || Entry.Identity)
				return std::unexpected(MakeError(AssetEditRecoveryJournalErrorCode::InvalidJournal, PathValue,
					"Absent file states must have zero length and no content fingerprint or asset identity"));
			return {};
		}

		bool IsKnownOperationKind(AssetEditRecoveryOperationKind Kind)
		{
			switch (Kind)
			{
			case AssetEditRecoveryOperationKind::AssetMove:
			case AssetEditRecoveryOperationKind::AssetBatchMove:
			case AssetEditRecoveryOperationKind::FolderMove:
			case AssetEditRecoveryOperationKind::AssetDuplicate:
			case AssetEditRecoveryOperationKind::FolderDuplicate:
				return true;
			}
			return false;
		}

		bool IsKnownState(AssetEditRecoveryState State)
		{
			switch (State)
			{
			case AssetEditRecoveryState::Prepared:
			case AssetEditRecoveryState::Applying:
			case AssetEditRecoveryState::Committed:
			case AssetEditRecoveryState::RollingBack:
				return true;
			}
			return false;
		}

		bool IsKnownActionKind(AssetEditRecoveryActionKind Kind)
		{
			switch (Kind)
			{
			case AssetEditRecoveryActionKind::Move:
			case AssetEditRecoveryActionKind::Rewrite:
			case AssetEditRecoveryActionKind::MoveAndRewrite:
			case AssetEditRecoveryActionKind::Create:
			case AssetEditRecoveryActionKind::DirectoryMove:
			case AssetEditRecoveryActionKind::DirectoryCreate:
				return true;
			}
			return false;
		}

		std::string_view ToString(AssetEditRecoveryOperationKind Kind)
		{
			switch (Kind)
			{
			case AssetEditRecoveryOperationKind::AssetMove:
				return "AssetMove";
			case AssetEditRecoveryOperationKind::AssetBatchMove:
				return "AssetBatchMove";
			case AssetEditRecoveryOperationKind::FolderMove:
				return "FolderMove";
			case AssetEditRecoveryOperationKind::AssetDuplicate:
				return "AssetDuplicate";
			case AssetEditRecoveryOperationKind::FolderDuplicate:
				return "FolderDuplicate";
			}
			return {};
		}

		std::string_view ToString(AssetEditRecoveryState State)
		{
			switch (State)
			{
			case AssetEditRecoveryState::Prepared:
				return "Prepared";
			case AssetEditRecoveryState::Applying:
				return "Applying";
			case AssetEditRecoveryState::Committed:
				return "Committed";
			case AssetEditRecoveryState::RollingBack:
				return "RollingBack";
			}
			return {};
		}

		std::string_view ToString(AssetEditRecoveryActionKind Kind)
		{
			switch (Kind)
			{
			case AssetEditRecoveryActionKind::Move:
				return "Move";
			case AssetEditRecoveryActionKind::Rewrite:
				return "Rewrite";
			case AssetEditRecoveryActionKind::MoveAndRewrite:
				return "MoveAndRewrite";
			case AssetEditRecoveryActionKind::Create:
				return "Create";
			case AssetEditRecoveryActionKind::DirectoryMove:
				return "DirectoryMove";
			case AssetEditRecoveryActionKind::DirectoryCreate:
				return "DirectoryCreate";
			}
			return {};
		}

		AssetEditRecoveryOperationKind ParseOperationKind(std::string_view Value)
		{
			for (const auto Kind : { AssetEditRecoveryOperationKind::AssetMove, AssetEditRecoveryOperationKind::AssetBatchMove,
					 AssetEditRecoveryOperationKind::FolderMove, AssetEditRecoveryOperationKind::AssetDuplicate,
					 AssetEditRecoveryOperationKind::FolderDuplicate })
				if (ToString(Kind) == Value) return Kind;
			throw std::runtime_error("Unknown recovery operation kind");
		}

		AssetEditRecoveryState ParseState(std::string_view Value)
		{
			for (const auto State : { AssetEditRecoveryState::Prepared, AssetEditRecoveryState::Applying, AssetEditRecoveryState::Committed,
					 AssetEditRecoveryState::RollingBack })
				if (ToString(State) == Value) return State;
			throw std::runtime_error("Unknown recovery journal state");
		}

		AssetEditRecoveryActionKind ParseActionKind(std::string_view Value)
		{
			for (const auto Kind : { AssetEditRecoveryActionKind::Move, AssetEditRecoveryActionKind::Rewrite,
					 AssetEditRecoveryActionKind::MoveAndRewrite, AssetEditRecoveryActionKind::Create,
					 AssetEditRecoveryActionKind::DirectoryMove, AssetEditRecoveryActionKind::DirectoryCreate })
				if (ToString(Kind) == Value) return Kind;
			throw std::runtime_error("Unknown recovery action kind");
		}

		AssetEditRecoveryEntryType ParseEntryType(std::string_view Value)
		{
			if (Value == "File") return AssetEditRecoveryEntryType::File;
			if (Value == "Directory") return AssetEditRecoveryEntryType::Directory;
			throw std::runtime_error("Unknown recovery entry type");
		}

		Json ParseUniqueJson(std::string_view Text)
		{
			constexpr int MaximumNestingDepth = 32;
			std::vector<std::set<std::string>> ObjectKeys;
			bool HasDuplicateKey = false;
			const auto Callback = [&ObjectKeys, &HasDuplicateKey](int Depth, Json::parse_event_t Event, Json& Parsed)
			{
				if (Depth > MaximumNestingDepth) throw std::runtime_error("Recovery journal JSON nesting exceeds the depth bound");
				if (Event == Json::parse_event_t::object_start) ObjectKeys.emplace_back();
				else if (Event == Json::parse_event_t::key && !ObjectKeys.empty())
					HasDuplicateKey |= !ObjectKeys.back().insert(Parsed.get<std::string>()).second;
				else if (Event == Json::parse_event_t::object_end && !ObjectKeys.empty()) ObjectKeys.pop_back();
				return true;
			};
			Json Document = Json::parse(Text, Callback);
			if (HasDuplicateKey) throw std::runtime_error("JSON object contains a duplicate key");
			return Document;
		}

		void RequireObjectKeys(const Json& Value, const std::set<std::string>& Expected, std::string_view Description)
		{
			if (!Value.is_object()) throw std::runtime_error(std::string(Description) + " must be an object");
			std::set<std::string> Actual;
			for (auto It = Value.begin(); It != Value.end(); ++It)
				Actual.insert(It.key());
			if (Actual != Expected) throw std::runtime_error(std::string(Description) + " has missing or unknown fields");
		}

		uint64_t ReadUnsigned(const Json& Value, std::string_view Description)
		{
			if (Value.is_number_unsigned()) return Value.get<uint64_t>();
			if (!Value.is_number_integer()) throw std::runtime_error(std::string(Description) + " must be an unsigned integer");
			const int64_t SignedValue = Value.get<int64_t>();
			if (SignedValue < 0) throw std::runtime_error(std::string(Description) + " must not be negative");
			return static_cast<uint64_t>(SignedValue);
		}

		UUID ParseUUID(std::string_view Text, std::string_view Description)
		{
			auto Parsed = UUID::Parse(Text);
			if (!Parsed || Parsed->IsNil() || Parsed->ToString() != Text)
				throw std::runtime_error(std::string(Description) + " is malformed or nil");
			return *Parsed;
		}

		Json SerializeFingerprint(const AssetContentFingerprint& Fingerprint)
		{
			return Json::object({ { "algorithm", Fingerprint.Algorithm }, { "digestHex", Fingerprint.DigestHex } });
		}

		Json SerializeEntryState(const AssetEditRecoveryEntryState& Entry)
		{
			Json Result = Json::object(
				{ { "type", Entry.Type == AssetEditRecoveryEntryType::File ? "File" : "Directory" }, { "exists", Entry.Exists } });
			if (Entry.Type == AssetEditRecoveryEntryType::File)
			{
				Result["byteLength"] = Entry.ByteLength;
				Result["fingerprint"] = Entry.Exists ? SerializeFingerprint(Entry.Content) : Json(nullptr);
				Result["assetId"] = Entry.Identity ? Json(Entry.Identity->ToString()) : Json(nullptr);
			}
			return Result;
		}

		AssetContentFingerprint DeserializeFingerprint(const Json& Value)
		{
			RequireObjectKeys(Value, { "algorithm", "digestHex" }, "Fingerprint");
			return { Value.at("algorithm").get<std::string>(), Value.at("digestHex").get<std::string>() };
		}

		AssetEditRecoveryEntryState DeserializeEntryState(const Json& Value)
		{
			if (!Value.is_object() || !Value.contains("type")) throw std::runtime_error("Entry state is missing its type");
			AssetEditRecoveryEntryState Entry;
			Entry.Type = ParseEntryType(Value.at("type").get<std::string>());
			if (Entry.Type == AssetEditRecoveryEntryType::Directory)
			{
				RequireObjectKeys(Value, { "type", "exists" }, "Directory state");
				Entry.Exists = Value.at("exists").get<bool>();
				return Entry;
			}
			RequireObjectKeys(Value, { "type", "exists", "byteLength", "fingerprint", "assetId" }, "File state");
			Entry.Exists = Value.at("exists").get<bool>();
			Entry.ByteLength = ReadUnsigned(Value.at("byteLength"), "File byteLength");
			if (!Value.at("fingerprint").is_null()) Entry.Content = DeserializeFingerprint(Value.at("fingerprint"));
			if (!Value.at("assetId").is_null()) Entry.Identity = ParseUUID(Value.at("assetId").get<std::string>(), "Asset identity");
			return Entry;
		}

		Json SerializeAction(const AssetEditRecoveryAction& Action)
		{
			return Json::object({ { "kind", std::string(ToString(Action.Kind)) }, { "originalPath", PathToUtf8(Action.OriginalPath) },
				{ "currentPath", PathToUtf8(Action.CurrentPath) },
				{ "transitPath", Action.TransitPath ? Json(PathToUtf8(*Action.TransitPath)) : Json(nullptr) },
				{ "stagedPath", Action.StagedPath ? Json(PathToUtf8(*Action.StagedPath)) : Json(nullptr) },
				{ "backupPath", Action.BackupPath ? Json(PathToUtf8(*Action.BackupPath)) : Json(nullptr) },
				{ "backupState", Action.BackupState ? SerializeEntryState(*Action.BackupState) : Json(nullptr) },
				{ "before", SerializeEntryState(Action.Before) }, { "after", SerializeEntryState(Action.After) } });
		}

		AssetEditRecoveryAction DeserializeAction(const Json& Value)
		{
			RequireObjectKeys(Value,
				{ "kind", "originalPath", "currentPath", "transitPath", "stagedPath", "backupPath", "backupState", "before", "after" },
				"Recovery action");
			AssetEditRecoveryAction Action;
			Action.Kind = ParseActionKind(Value.at("kind").get<std::string>());
			Action.OriginalPath = PathFromUtf8(Value.at("originalPath").get<std::string>());
			Action.CurrentPath = PathFromUtf8(Value.at("currentPath").get<std::string>());
			if (!Value.at("transitPath").is_null()) Action.TransitPath = PathFromUtf8(Value.at("transitPath").get<std::string>());
			if (!Value.at("stagedPath").is_null()) Action.StagedPath = PathFromUtf8(Value.at("stagedPath").get<std::string>());
			if (!Value.at("backupPath").is_null()) Action.BackupPath = PathFromUtf8(Value.at("backupPath").get<std::string>());
			if (!Value.at("backupState").is_null()) Action.BackupState = DeserializeEntryState(Value.at("backupState"));
			Action.Before = DeserializeEntryState(Value.at("before"));
			Action.After = DeserializeEntryState(Value.at("after"));
			return Action;
		}

		Json SerializeRecord(const AssetEditRecoveryJournalRecord& Record)
		{
			Json Completed = Json::array();
			for (const uint32_t Index : Record.CompletedActions)
				Completed.push_back(Index);
			Json Actions = Json::array();
			for (const AssetEditRecoveryAction& Action : Record.Actions)
				Actions.push_back(SerializeAction(Action));
			return Json::object({ { "format", std::string(JournalFormat) }, { "version", Record.Version },
				{ "operationId", Record.OperationID.ToString() }, { "projectId", Record.ProjectID.ToString() },
				{ "projectRootUtf8", Record.ProjectRootUtf8 }, { "operationKind", ToString(Record.OperationKind) },
				{ "state", ToString(Record.State) }, { "completedActions", std::move(Completed) }, { "actions", std::move(Actions) } });
		}

		AssetEditRecoveryJournalRecord DeserializeRecord(const Json& Value)
		{
			RequireObjectKeys(Value,
				{ "format", "version", "operationId", "projectId", "projectRootUtf8", "operationKind", "state", "completedActions",
					"actions" },
				"Recovery journal");
			if (Value.at("format").get<std::string>() != JournalFormat)
				throw std::runtime_error("Recovery journal format identifier is invalid");
			AssetEditRecoveryJournalRecord Record;
			const uint64_t Version = ReadUnsigned(Value.at("version"), "Journal version");
			if (Version > std::numeric_limits<uint32_t>::max()) throw std::runtime_error("Journal version is out of range");
			Record.Version = static_cast<uint32_t>(Version);
			Record.OperationID = ParseUUID(Value.at("operationId").get<std::string>(), "Operation identity");
			Record.ProjectID = ParseUUID(Value.at("projectId").get<std::string>(), "Project identity");
			Record.ProjectRootUtf8 = Value.at("projectRootUtf8").get<std::string>();
			Record.OperationKind = ParseOperationKind(Value.at("operationKind").get<std::string>());
			Record.State = ParseState(Value.at("state").get<std::string>());
			if (!Value.at("completedActions").is_array()) throw std::runtime_error("completedActions must be an array");
			if (!Value.at("actions").is_array()) throw std::runtime_error("actions must be an array");
			if (Value.at("completedActions").size() > AssetEditRecoveryJournal::MaximumActions ||
				Value.at("actions").size() > AssetEditRecoveryJournal::MaximumActions)
				throw std::runtime_error("Recovery journal exceeds the action count bound");
			for (const Json& IndexValue : Value.at("completedActions"))
			{
				const uint64_t Index = ReadUnsigned(IndexValue, "Completed action index");
				if (Index > std::numeric_limits<uint32_t>::max()) throw std::runtime_error("Completed action index is out of range");
				Record.CompletedActions.push_back(static_cast<uint32_t>(Index));
			}
			for (const Json& ActionValue : Value.at("actions"))
				Record.Actions.push_back(DeserializeAction(ActionValue));
			return Record;
		}

		std::expected<void, AssetEditRecoveryJournalError> ValidateJournalLocation(
			const Path& JournalPath, const AssetEditRecoveryJournalRecord& Record, bool MustExist)
		{
			auto Root = ParseProjectRoot(Record.ProjectRootUtf8);
			if (!Root) return std::unexpected(Root.error());
			if (JournalPath.empty() || !JournalPath.is_absolute() || JournalPath.lexically_normal() != JournalPath ||
				PathToUtf8(JournalPath.filename()) != JournalFilename)
				return std::unexpected(MakeError(AssetEditRecoveryJournalErrorCode::InvalidJournal, JournalPath,
					"Journal must be RecoveryRecord.json at a normalized absolute path"));
			const Path TransactionDirectory = JournalPath.parent_path();
			const Path ExpectedTransactionDirectory = *Root / (std::string(TransactionDirectoryPrefix) + Record.OperationID.ToString());
			if (TransactionDirectory != ExpectedTransactionDirectory || TransactionDirectory.parent_path() != *Root)
				return std::unexpected(MakeError(AssetEditRecoveryJournalErrorCode::InvalidJournal, JournalPath,
					"RecoveryRecord.json must be directly inside its operation-owned transaction directory"));
			if (auto Checked = ValidatePathAncestors(*Root, false, FinalPathKind::Directory, true); !Checked) return Checked;
			if (auto Checked = ValidatePathAncestors(TransactionDirectory, false, FinalPathKind::Directory, true); !Checked) return Checked;
			return ValidatePathAncestors(JournalPath, false, FinalPathKind::File, MustExist);
		}

		std::expected<void, AssetEditRecoveryJournalError> ValidateRecordedPathOnDisk(const Path& Root, const Path& Relative)
		{
			return ValidatePathAncestors(Root / Relative, true, FinalPathKind::Any, false);
		}

		std::expected<void, std::string> WriteExclusiveFile(const Path& TemporaryPath, std::string_view Contents)
		{
#if defined(_WIN32)
			HANDLE File = CreateFileW(TemporaryPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (File == INVALID_HANDLE_VALUE)
				return std::unexpected(
					"Could not exclusively create journal temporary file (Windows error " + std::to_string(GetLastError()) + ")");
			size_t Offset = 0;
			while (Offset < Contents.size())
			{
				const DWORD Request = static_cast<DWORD>(std::min<size_t>(Contents.size() - Offset, std::numeric_limits<DWORD>::max()));
				DWORD Written = 0;
				if (!WriteFile(File, Contents.data() + Offset, Request, &Written, nullptr) || Written == 0)
				{
					const DWORD Failure = GetLastError();
					CloseHandle(File);
					std::error_code CleanupError;
					std::filesystem::remove(TemporaryPath, CleanupError);
					std::string Message = "Could not write journal temporary file (Windows error " + std::to_string(Failure) + ")";
					if (CleanupError) Message += "; temporary cleanup failed: " + CleanupError.message();
					return std::unexpected(std::move(Message));
				}
				Offset += Written;
			}
			if (!CloseHandle(File))
			{
				const DWORD Failure = GetLastError();
				std::error_code CleanupError;
				std::filesystem::remove(TemporaryPath, CleanupError);
				std::string Message = "Could not close journal temporary file (Windows error " + std::to_string(Failure) + ")";
				if (CleanupError) Message += "; temporary cleanup failed: " + CleanupError.message();
				return std::unexpected(std::move(Message));
			}
			return {};
#elif defined(__APPLE__) || defined(__linux__)
			const int File = open(TemporaryPath.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
			if (File < 0)
				return std::unexpected("Could not exclusively create journal temporary file: " + std::string(std::strerror(errno)));
			size_t Offset = 0;
			while (Offset < Contents.size())
			{
				const ssize_t Written = write(File, Contents.data() + Offset, Contents.size() - Offset);
				if (Written < 0 && errno == EINTR) continue;
				if (Written <= 0)
				{
					const std::string Failure = std::strerror(errno);
					close(File);
					std::error_code CleanupError;
					std::filesystem::remove(TemporaryPath, CleanupError);
					std::string Message = "Could not write journal temporary file: " + Failure;
					if (CleanupError) Message += "; temporary cleanup failed: " + CleanupError.message();
					return std::unexpected(std::move(Message));
				}
				Offset += static_cast<size_t>(Written);
			}
			if (close(File) != 0)
			{
				const std::string Failure = std::strerror(errno);
				std::error_code CleanupError;
				std::filesystem::remove(TemporaryPath, CleanupError);
				std::string Message = "Could not close journal temporary file: " + Failure;
				if (CleanupError) Message += "; temporary cleanup failed: " + CleanupError.message();
				return std::unexpected(std::move(Message));
			}
			return {};
#else
			(void)TemporaryPath;
			(void)Contents;
			return std::unexpected("Exclusive journal writes are unsupported on this platform");
#endif
		}
	} // namespace

	std::expected<void, AssetEditRecoveryJournalError> AssetEditRecoveryJournal::Validate(const AssetEditRecoveryJournalRecord& Record)
	{
		if (Record.Version != CurrentVersion)
			return std::unexpected(
				MakeError(AssetEditRecoveryJournalErrorCode::UnsupportedVersion, {}, "Recovery journal version is not supported"));
		if (Record.OperationID.IsNil() || Record.ProjectID.IsNil())
			return std::unexpected(
				MakeError(AssetEditRecoveryJournalErrorCode::InvalidJournal, {}, "Operation and project identities must be non-nil UUIDs"));
		if (!IsKnownOperationKind(Record.OperationKind) || !IsKnownState(Record.State))
			return std::unexpected(MakeError(
				AssetEditRecoveryJournalErrorCode::InvalidJournal, {}, "Recovery journal contains an unknown operation kind or state"));
		if (Record.Actions.size() > MaximumActions || Record.CompletedActions.size() > MaximumActions)
			return std::unexpected(
				MakeError(AssetEditRecoveryJournalErrorCode::InvalidJournal, {}, "Recovery journal exceeds the action count bound"));

		auto Root = ParseProjectRoot(Record.ProjectRootUtf8);
		if (!Root) return std::unexpected(Root.error());
		if (auto Checked = ValidatePathAncestors(*Root, false, FinalPathKind::Directory, true); !Checked) return Checked;

		std::set<uint32_t> Completed;
		for (const uint32_t Index : Record.CompletedActions)
			if (Index >= Record.Actions.size() || !Completed.insert(Index).second)
				return std::unexpected(MakeError(AssetEditRecoveryJournalErrorCode::InvalidJournal, {},
					"Completed action indices must be unique and refer to an action in this record"));

		std::unordered_map<std::string, size_t> LogicalPathOwners;
		std::set<std::string> BackupPaths;
		for (size_t Index = 0; Index < Record.Actions.size(); ++Index)
		{
			const AssetEditRecoveryAction& Action = Record.Actions[Index];
			if (!IsKnownActionKind(Action.Kind))
				return std::unexpected(
					MakeError(AssetEditRecoveryJournalErrorCode::InvalidJournal, Action.CurrentPath, "Recovery action kind is unknown"));
			const bool IsDirectoryAction =
				Action.Kind == AssetEditRecoveryActionKind::DirectoryMove || Action.Kind == AssetEditRecoveryActionKind::DirectoryCreate;
			const auto RequireType = [IsDirectoryAction](const AssetEditRecoveryEntryState& Entry)
			{ return Entry.Type == (IsDirectoryAction ? AssetEditRecoveryEntryType::Directory : AssetEditRecoveryEntryType::File); };
			if (!RequireType(Action.Before) || !RequireType(Action.After))
				return std::unexpected(MakeError(AssetEditRecoveryJournalErrorCode::InvalidJournal, Action.CurrentPath,
					"Action kind and before/after entry types disagree"));
			if (auto Checked = ValidateEntryState(Action.Before, Action.OriginalPath.empty() ? Action.CurrentPath : Action.OriginalPath);
				!Checked)
				return Checked;
			if (auto Checked = ValidateEntryState(Action.After, Action.CurrentPath); !Checked) return Checked;

			const bool IsCreate =
				Action.Kind == AssetEditRecoveryActionKind::Create || Action.Kind == AssetEditRecoveryActionKind::DirectoryCreate;
			const bool IsRewrite = Action.Kind == AssetEditRecoveryActionKind::Rewrite;
			const bool IsMove =
				Action.Kind == AssetEditRecoveryActionKind::Move || Action.Kind == AssetEditRecoveryActionKind::DirectoryMove;
			const bool IsMoveAndRewrite = Action.Kind == AssetEditRecoveryActionKind::MoveAndRewrite;
			if (!IsCreate)
			{
				if (auto Checked = ValidateProjectRelativePath(Action.OriginalPath, false, true); !Checked) return Checked;
			}
			else if (auto Checked = ValidateProjectRelativePath(Action.OriginalPath, true, true); !Checked) return Checked;
			if (auto Checked = ValidateProjectRelativePath(Action.CurrentPath, false, true); !Checked) return Checked;

			if (IsCreate)
			{
				if (!Action.After.Exists || Action.Before.Exists != !Action.OriginalPath.empty() ||
					(!Action.OriginalPath.empty() && Action.OriginalPath == Action.CurrentPath))
					return std::unexpected(MakeError(AssetEditRecoveryJournalErrorCode::InvalidJournal, Action.CurrentPath,
						"Create actions require an absent source or an existing distinct source and a present destination"));
			}
			else if (!Action.Before.Exists || !Action.After.Exists)
				return std::unexpected(MakeError(AssetEditRecoveryJournalErrorCode::InvalidJournal, Action.CurrentPath,
					"Move and rewrite actions must transition between present entries"));

			if (IsRewrite && Action.OriginalPath != Action.CurrentPath)
				return std::unexpected(MakeError(AssetEditRecoveryJournalErrorCode::InvalidJournal, Action.CurrentPath,
					"Rewrite actions must keep the same logical path"));
			if ((IsMove || IsMoveAndRewrite) && Action.OriginalPath == Action.CurrentPath)
				return std::unexpected(MakeError(
					AssetEditRecoveryJournalErrorCode::InvalidJournal, Action.CurrentPath, "Move actions must change the logical path"));
			const bool IsMoveAction = IsMove || IsMoveAndRewrite;
			const bool HasCaseOnlySpelling =
				Action.OriginalPath != Action.CurrentPath &&
				AsciiCaseFoldedPathSpellingKey(Action.OriginalPath) == AsciiCaseFoldedPathSpellingKey(Action.CurrentPath);
			const bool IsCaseOnlyMove =
				IsMoveAction && HasCaseOnlySpelling &&
				(Action.TransitPath.has_value() || LogicalPathKey(Action.OriginalPath) == LogicalPathKey(Action.CurrentPath));
			if (IsCaseOnlyMove && !Action.TransitPath)
				return std::unexpected(MakeError(AssetEditRecoveryJournalErrorCode::InvalidJournal, Action.CurrentPath,
					"Case-only moves require a distinct transit path"));
			if (Action.BackupPath && IsDirectoryAction)
				return std::unexpected(MakeError(
					AssetEditRecoveryJournalErrorCode::InvalidJournal, Action.CurrentPath, "Directory actions cannot have file backups"));
			if (Action.TransitPath)
			{
				if (!(IsMove || IsMoveAndRewrite))
					return std::unexpected(MakeError(AssetEditRecoveryJournalErrorCode::InvalidJournal, *Action.TransitPath,
						"Transit paths are only valid for move actions"));
				if (!IsCaseOnlyMove)
					return std::unexpected(MakeError(AssetEditRecoveryJournalErrorCode::InvalidJournal, *Action.TransitPath,
						"Transit paths are only valid for case-only moves"));
				if (auto Checked = ValidateProjectRelativePath(*Action.TransitPath, false, true); !Checked) return Checked;
				if (LogicalPathKey(*Action.TransitPath) == LogicalPathKey(Action.OriginalPath) ||
					LogicalPathKey(*Action.TransitPath) == LogicalPathKey(Action.CurrentPath))
					return std::unexpected(MakeError(AssetEditRecoveryJournalErrorCode::InvalidJournal, *Action.TransitPath,
						"Transit path must be distinct from its logical source and destination"));
			}
			if (Action.StagedPath)
			{
				if (auto Checked = ValidateTransactionRelativePath(*Action.StagedPath); !Checked) return Checked;
				if (!BackupPaths.insert("stage:" + LogicalPathKey(*Action.StagedPath)).second)
					return std::unexpected(MakeError(AssetEditRecoveryJournalErrorCode::InvalidJournal, *Action.StagedPath,
						"Staged path is assigned to more than one action"));
				const Path StagedAbsolute =
					*Root / (std::string(TransactionDirectoryPrefix) + Record.OperationID.ToString()) / *Action.StagedPath;
				if (auto Checked = ValidatePathAncestors(StagedAbsolute, true, FinalPathKind::Any, false); !Checked) return Checked;
			}
			if (Action.BackupPath)
			{
				if (IsDirectoryAction)
					return std::unexpected(MakeError(AssetEditRecoveryJournalErrorCode::InvalidJournal, *Action.BackupPath,
						"Directory actions cannot have file backups"));
				if (auto Checked = ValidateTransactionRelativePath(*Action.BackupPath); !Checked) return Checked;
				if (!BackupPaths.insert(LogicalPathKey(*Action.BackupPath)).second)
					return std::unexpected(MakeError(AssetEditRecoveryJournalErrorCode::InvalidJournal, *Action.BackupPath,
						"Backup path is assigned to more than one action"));
				if (!Action.BackupState || Action.BackupState->Type != AssetEditRecoveryEntryType::File || !Action.BackupState->Exists)
					return std::unexpected(MakeError(AssetEditRecoveryJournalErrorCode::InvalidJournal, *Action.BackupPath,
						"Backup paths require an expected existing file receipt"));
				if (auto Checked = ValidateEntryState(*Action.BackupState, *Action.BackupPath); !Checked) return Checked;
			}
			else if (Action.BackupState)
				return std::unexpected(MakeError(AssetEditRecoveryJournalErrorCode::InvalidJournal, Action.CurrentPath,
					"Backup receipt has no corresponding backup path"));

			if (Action.Before.Identity.has_value() != Action.After.Identity.has_value() && !IsCreate)
				return std::unexpected(MakeError(AssetEditRecoveryJournalErrorCode::InvalidJournal, Action.CurrentPath,
					"Move or rewrite identity must be present both before and after, or absent both times"));
			if (!IsCreate && Action.Before.Identity && Action.After.Identity && *Action.Before.Identity != *Action.After.Identity)
				return std::unexpected(MakeError(AssetEditRecoveryJournalErrorCode::InvalidJournal, Action.CurrentPath,
					"Move and rewrite actions must preserve asset identity"));
			if (Action.Kind == AssetEditRecoveryActionKind::Move &&
				(Action.Before.ByteLength != Action.After.ByteLength || Action.Before.Content != Action.After.Content))
				return std::unexpected(MakeError(AssetEditRecoveryJournalErrorCode::InvalidJournal, Action.CurrentPath,
					"Move actions must preserve file length and content fingerprint"));

			std::vector<Path> ActionPaths;
			if (!Action.OriginalPath.empty()) ActionPaths.push_back(Action.OriginalPath);
			ActionPaths.push_back(Action.CurrentPath);
			if (Action.TransitPath) ActionPaths.push_back(*Action.TransitPath);
			std::set<std::string> PerActionPaths;
			for (const Path& Relative : ActionPaths)
			{
				const std::string Key = LogicalPathKey(Relative);
				if (!PerActionPaths.insert(Key).second)
				{
					if ((IsRewrite || (IsCaseOnlyMove && Relative == Action.CurrentPath)) && Key == LogicalPathKey(Action.OriginalPath) &&
						Key == LogicalPathKey(Action.CurrentPath))
					{
						if (auto Checked = ValidateRecordedPathOnDisk(*Root, Relative); !Checked) return Checked;
						continue;
					}
					return std::unexpected(MakeError(
						AssetEditRecoveryJournalErrorCode::InvalidJournal, Relative, "One action contains conflicting logical paths"));
				}
				auto [Owner, Inserted] = LogicalPathOwners.emplace(Key, Index);
				if (!Inserted && Owner->second != Index)
					return std::unexpected(MakeError(AssetEditRecoveryJournalErrorCode::InvalidJournal, Relative,
						"Logical path is used by more than one recovery action"));
				if (auto Checked = ValidateRecordedPathOnDisk(*Root, Relative); !Checked) return Checked;
			}
			if (Action.BackupPath)
			{
				const Path BackupAbsolute =
					*Root / (std::string(TransactionDirectoryPrefix) + Record.OperationID.ToString()) / *Action.BackupPath;
				if (auto Checked = ValidatePathAncestors(BackupAbsolute, true, FinalPathKind::Any, false); !Checked) return Checked;
			}
		}

		return {};
	}

	std::expected<AssetEditRecoveryJournalRecord, AssetEditRecoveryJournalError> AssetEditRecoveryJournal::Read(
		const std::filesystem::path& JournalPath)
	{
		if (JournalPath.empty() || !JournalPath.is_absolute() || JournalPath.lexically_normal() != JournalPath ||
			PathToUtf8(JournalPath.filename()) != JournalFilename ||
			!PathToUtf8(JournalPath.parent_path().filename()).starts_with(TransactionDirectoryPrefix))
			return std::unexpected(MakeError(AssetEditRecoveryJournalErrorCode::InvalidJournal, JournalPath,
				"Journal must be RecoveryRecord.json directly under an asset-edit transaction directory"));
		if (auto Safe = ValidatePathAncestors(JournalPath, false, FinalPathKind::File, true); !Safe) return std::unexpected(Safe.error());

		std::error_code Error;
		const uintmax_t Length = std::filesystem::file_size(JournalPath, Error);
		if (Error || Length > MaximumDocumentBytes)
			return std::unexpected(MakeError(AssetEditRecoveryJournalErrorCode::ReadFailed, JournalPath,
				Error ? "Could not inspect recovery journal size: " + Error.message()
					  : "Recovery journal exceeds the document size bound"));
		std::string Contents(static_cast<size_t>(Length), '\0');
		std::ifstream Input(JournalPath, std::ios::binary);
		if (!Input || (!Contents.empty() && !Input.read(Contents.data(), static_cast<std::streamsize>(Contents.size()))) || Input.bad())
			return std::unexpected(
				MakeError(AssetEditRecoveryJournalErrorCode::ReadFailed, JournalPath, "Could not read the complete recovery journal"));
		if (Input.peek() != std::char_traits<char>::eof())
			return std::unexpected(MakeError(AssetEditRecoveryJournalErrorCode::ReadFailed, JournalPath,
				"Recovery journal changed while being read or exceeds its size bound"));

		try
		{
			AssetEditRecoveryJournalRecord Record = DeserializeRecord(ParseUniqueJson(Contents));
			if (auto Valid = Validate(Record); !Valid) return std::unexpected(Valid.error());
			if (auto Location = ValidateJournalLocation(JournalPath, Record, true); !Location) return std::unexpected(Location.error());
			return Record;
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(MakeError(AssetEditRecoveryJournalErrorCode::InvalidJournal, JournalPath,
				"Could not decode recovery journal: " + std::string(Exception.what())));
		}
	}

	std::expected<void, AssetEditRecoveryJournalError> AssetEditRecoveryJournal::WriteAtomic(
		const std::filesystem::path& JournalPath, const AssetEditRecoveryJournalRecord& Record)
	{
		if (auto Valid = Validate(Record); !Valid) return Valid;
		if (auto Location = ValidateJournalLocation(JournalPath, Record, false); !Location) return Location;

		std::string Contents;
		try
		{
			Contents = SerializeRecord(Record).dump(2, ' ', false, Json::error_handler_t::strict) + "\n";
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(MakeError(AssetEditRecoveryJournalErrorCode::WriteFailed, JournalPath,
				"Could not encode recovery journal: " + std::string(Exception.what())));
		}
		if (Contents.size() > MaximumDocumentBytes)
			return std::unexpected(
				MakeError(AssetEditRecoveryJournalErrorCode::WriteFailed, JournalPath, "Recovery journal exceeds the document size bound"));

		auto TemporaryID = UUID::Generate();
		if (!TemporaryID)
			return std::unexpected(MakeError(
				AssetEditRecoveryJournalErrorCode::WriteFailed, JournalPath, "Could not generate a unique journal temporary identity"));
		Path TemporaryPath = JournalPath;
		TemporaryPath += "." + TemporaryID->ToString() + ".tmp";
		if (auto Safe = ValidatePathAncestors(TemporaryPath, false, FinalPathKind::File, false); !Safe)
			return std::unexpected(Safe.error());
		if (auto Written = WriteExclusiveFile(TemporaryPath, Contents); !Written)
			return std::unexpected(MakeError(AssetEditRecoveryJournalErrorCode::WriteFailed, TemporaryPath, Written.error()));

		if (auto Replaced = Platform::ReplaceFileAtomically(TemporaryPath, JournalPath); !Replaced)
		{
			std::error_code CleanupError;
			std::filesystem::remove(TemporaryPath, CleanupError);
			std::string Message = "Could not atomically replace recovery journal: " + Replaced.error().Message;
			if (CleanupError) Message += "; temporary cleanup failed: " + CleanupError.message();
			return std::unexpected(MakeError(AssetEditRecoveryJournalErrorCode::WriteFailed, JournalPath, std::move(Message)));
		}
		return {};
	}
} // namespace PulseForge::Assets::Detail
