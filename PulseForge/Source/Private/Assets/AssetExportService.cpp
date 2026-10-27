#include "Assets/AssetExportService.h"

#include "Assets/AssetDependencyCatalog.h"
#include "Assets/AssetImportProvenance.h"
#include "Assets/Project.h"
#include "Core/PlatformServices.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <system_error>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace PulseForge
{
	namespace
	{
		using Json = nlohmann::ordered_json;
		constexpr size_t MaximumDescriptorBytes = 1024u * 1024u;
		constexpr std::string_view InventoryName = ".pf-export-inventory.json";
		constexpr std::string_view RuntimeConfigurationName = "PulseForgeRuntime.cfg";
		constexpr std::string_view RecoveryFormat = "PulseForgeExportRecovery";
		constexpr uint32_t RecoveryVersion = 1;
		constexpr uint64_t MaximumRecoveryBytes = 16u * 1024u * 1024u;
		constexpr std::string_view RecoveryStatePrepared = "Prepared";
		constexpr std::string_view RecoveryStateStaging = "Staging";
		constexpr std::string_view RecoveryStateOldMoved = "OldMoved";
		constexpr std::string_view RecoveryStateNewPublished = "NewPublished";
		constexpr std::string_view RecoveryStateCleanupPending = "CleanupPending";

		struct ExportRecoveryRecord final
		{
			std::string OperationID;
			std::string ProjectID;
			std::filesystem::path ProjectRoot;
			std::filesystem::path Output;
			std::filesystem::path Candidate;
			std::filesystem::path Backup;
			bool HadPreviousOutput = false;
			std::string State;
			std::map<std::string, std::string> PreviousTree;
			std::map<std::string, std::string> CandidateTree;
		};

		class ExportProcessLock final
		{
		public:
			explicit ExportProcessLock(const std::filesystem::path& Path)
			{
#if defined(_WIN32)
				m_Handle = CreateFileW(Path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
					FILE_ATTRIBUTE_NORMAL, nullptr);
				m_Acquired = m_Handle != INVALID_HANDLE_VALUE;
#else
				m_Descriptor = open(Path.c_str(), O_CREAT | O_RDWR, 0600);
				m_Acquired = m_Descriptor >= 0 && flock(m_Descriptor, LOCK_EX | LOCK_NB) == 0;
				if (!m_Acquired && m_Descriptor >= 0)
				{
					close(m_Descriptor);
					m_Descriptor = -1;
				}
#endif
			}

			~ExportProcessLock()
			{
#if defined(_WIN32)
				if (m_Acquired) CloseHandle(m_Handle);
#else
				if (m_Descriptor >= 0) close(m_Descriptor);
#endif
			}

			[[nodiscard]] bool Acquired() const noexcept { return m_Acquired; }

		private:
			bool m_Acquired = false;
#if defined(_WIN32)
			HANDLE m_Handle = INVALID_HANDLE_VALUE;
#else
			int m_Descriptor = -1;
#endif
		};

		AssetExportError MakeError(AssetExportErrorCode Code, std::string Message, std::filesystem::path Path = {})
		{
			return { Code, std::move(Path), std::move(Message) };
		}

		std::string PathUtf8(const std::filesystem::path& Path)
		{
			const auto Value = Path.u8string();
			return { reinterpret_cast<const char*>(Value.data()), Value.size() };
		}

		std::expected<std::filesystem::path, AssetExportError> Absolute(const std::filesystem::path& Path)
		{
			std::error_code Error;
			auto Result = std::filesystem::absolute(Path, Error);
			if (Error) return std::unexpected(MakeError(AssetExportErrorCode::InvalidRequest,
				"Could not resolve path: " + Error.message(), Path));
			return Result.lexically_normal();
		}

		bool Contains(const std::filesystem::path& Parent, const std::filesystem::path& Child)
		{
			auto P = Parent.begin();
			auto C = Child.begin();
			for (; P != Parent.end(); ++P, ++C)
				if (C == Child.end() || *P != *C) return false;
			return true;
		}

		bool IsSafeRelative(std::string_view Value)
		{
			if (Value.empty() || Value.size() > 4096 || Value.front() == '/' || Value.find('\\') != std::string_view::npos)
				return false;
			std::filesystem::path Path = std::filesystem::u8path(Value);
			if (Path.is_absolute() || Path.has_root_name() || Path.has_root_directory()) return false;
			for (const auto& Part : Path)
				if (Part.empty() || Part == "." || Part == "..") return false;
			return true;
		}

		std::expected<void, AssetExportError> CheckNoLinks(const std::filesystem::path& Path)
		{
			std::filesystem::path Current;
			std::error_code Error;
			for (const auto& Part : Path)
			{
				Current /= Part;
				auto Status = std::filesystem::symlink_status(Current, Error);
				if (Error == std::errc::no_such_file_or_directory) { Error.clear(); continue; }
				if (Error) return std::unexpected(MakeError(AssetExportErrorCode::InvalidRequest,
					"Could not inspect path component: " + Error.message(), Current));
				if (std::filesystem::is_symlink(Status))
					return std::unexpected(MakeError(AssetExportErrorCode::InvalidRequest,
						"Export paths may not traverse a symlink or reparse point", Current));
			}
			return {};
		}

		std::expected<std::vector<std::byte>, AssetExportError> ReadFile(const std::filesystem::path& Path, uint64_t Limit)
		{
			std::error_code Error;
			const uintmax_t Size = std::filesystem::file_size(Path, Error);
			if (Error || Size > Limit || Size > static_cast<uintmax_t>((std::numeric_limits<size_t>::max)()))
				return std::unexpected(MakeError(AssetExportErrorCode::VerificationFailed,
					Error ? "Could not inspect file: " + Error.message() : "File exceeds the export size limit", Path));
			std::ifstream Input(Path, std::ios::binary);
			if (!Input) return std::unexpected(MakeError(AssetExportErrorCode::VerificationFailed, "Could not open file", Path));
			std::vector<std::byte> Bytes(static_cast<size_t>(Size));
			if (!Bytes.empty()) Input.read(reinterpret_cast<char*>(Bytes.data()), static_cast<std::streamsize>(Bytes.size()));
			if (!Input || Input.bad()) return std::unexpected(MakeError(AssetExportErrorCode::VerificationFailed, "Could not read file", Path));
			return Bytes;
		}

		std::expected<AssetContentFingerprint, AssetExportError> Fingerprint(const std::filesystem::path& Path)
		{
			auto Bytes = ReadFile(Path, MaximumExportFileBytes);
			if (!Bytes) return std::unexpected(Bytes.error());
			return ComputeAssetContentFingerprint(*Bytes);
		}

		std::expected<void, AssetExportError> CopyOne(const std::filesystem::path& Source,
			const std::filesystem::path& Destination)
		{
			std::error_code Error;
			std::filesystem::create_directories(Destination.parent_path(), Error);
			if (Error) return std::unexpected(MakeError(AssetExportErrorCode::StageFailed,
				"Could not create package directory: " + Error.message(), Destination.parent_path()));
			if (!std::filesystem::copy_file(Source, Destination, std::filesystem::copy_options::overwrite_existing, Error))
				return std::unexpected(MakeError(AssetExportErrorCode::StageFailed,
					"Could not copy runtime file: " + Error.message(), Source));
#if !defined(_WIN32)
			const auto SourceStatus = std::filesystem::status(Source, Error);
			if (Error) return std::unexpected(MakeError(AssetExportErrorCode::StageFailed,
				"Could not read runtime file permissions: " + Error.message(), Source));
			std::filesystem::permissions(Destination, SourceStatus.permissions(), std::filesystem::perm_options::replace, Error);
			if (Error) return std::unexpected(MakeError(AssetExportErrorCode::StageFailed,
				"Could not preserve runtime file permissions: " + Error.message(), Destination));
#endif
			return {};
		}

		std::expected<std::vector<std::string>, AssetExportError> EnumerateFiles(const std::filesystem::path& Root)
		{
			std::vector<std::string> Files;
			std::error_code Error;
			for (std::filesystem::recursive_directory_iterator It(Root, std::filesystem::directory_options::none, Error), End;
				It != End; It.increment(Error))
			{
				if (Error) return std::unexpected(MakeError(AssetExportErrorCode::VerificationFailed,
					"Could not enumerate package: " + Error.message(), It->path()));
				const auto Status = It->symlink_status(Error);
				if (Error || std::filesystem::is_symlink(Status))
					return std::unexpected(MakeError(AssetExportErrorCode::VerificationFailed,
						"Package contains an unreadable or linked entry", It->path()));
				if (!std::filesystem::is_regular_file(Status)) continue;
				auto Relative = std::filesystem::relative(It->path(), Root, Error);
				if (Error) return std::unexpected(MakeError(AssetExportErrorCode::VerificationFailed,
					"Could not form package-relative path", It->path()));
				const auto U8 = Relative.generic_u8string();
				std::string Name(reinterpret_cast<const char*>(U8.data()), U8.size());
				if (!IsSafeRelative(Name)) return std::unexpected(MakeError(AssetExportErrorCode::VerificationFailed,
					"Package contains an unsafe relative path", It->path()));
				Files.push_back(std::move(Name));
				if (Files.size() > MaximumExportFileCount)
					return std::unexpected(MakeError(AssetExportErrorCode::VerificationFailed, "Package file count exceeds its limit", Root));
			}
			std::sort(Files.begin(), Files.end());
			return Files;
		}

		std::expected<std::map<std::string, std::string>, AssetExportError> CaptureTree(const std::filesystem::path& Root)
		{
			std::map<std::string, std::string> Result;
			std::error_code Error;
			for (std::filesystem::recursive_directory_iterator It(Root, std::filesystem::directory_options::none, Error), End;
				It != End; It.increment(Error))
			{
				if (Error) return std::unexpected(MakeError(AssetExportErrorCode::VerificationFailed,
					"Could not enumerate export tree: " + Error.message(), It->path()));
				const auto Status = It->symlink_status(Error);
				if (Error || std::filesystem::is_symlink(Status) ||
					(!std::filesystem::is_directory(Status) && !std::filesystem::is_regular_file(Status)))
					return std::unexpected(MakeError(AssetExportErrorCode::VerificationFailed,
						"Export tree contains an unreadable, linked, or unsupported filesystem object", It->path()));
				auto Relative = std::filesystem::relative(It->path(), Root, Error);
				if (Error) return std::unexpected(MakeError(AssetExportErrorCode::VerificationFailed,
					"Could not form export-relative path", It->path()));
				const auto U8 = Relative.generic_u8string();
				const std::string Name(reinterpret_cast<const char*>(U8.data()), U8.size());
				if (!IsSafeRelative(Name)) return std::unexpected(MakeError(AssetExportErrorCode::VerificationFailed,
					"Export tree contains an unsafe path", It->path()));
				if (std::filesystem::is_directory(Status))
					Result.emplace("D:" + Name, "directory");
				else
				{
					auto Digest = Fingerprint(It->path());
					if (!Digest) return std::unexpected(Digest.error());
					Result.emplace("F:" + Name, Digest->DigestHex);
				}
				if (Result.size() > MaximumExportFileCount * 2)
					return std::unexpected(MakeError(AssetExportErrorCode::VerificationFailed,
						"Export tree exceeds its entry limit", Root));
			}
			return Result;
		}

		std::filesystem::path GetRecoveryPath(const std::filesystem::path& Output)
		{
			return Output.parent_path() / ("." + PathUtf8(Output.filename()) + ".pf-export-recovery.json");
		}

		Json SerializeRecoveryRecord(const ExportRecoveryRecord& Record)
		{
			Json Value;
			Value["format"] = RecoveryFormat;
			Value["version"] = RecoveryVersion;
			Value["operationId"] = Record.OperationID;
			Value["projectId"] = Record.ProjectID;
			Value["projectRoot"] = PathUtf8(Record.ProjectRoot);
			Value["output"] = PathUtf8(Record.Output);
			Value["candidate"] = PathUtf8(Record.Candidate);
			Value["backup"] = PathUtf8(Record.Backup);
			Value["hadPreviousOutput"] = Record.HadPreviousOutput;
			Value["state"] = Record.State;
			Value["previousTree"] = Record.PreviousTree;
			Value["candidateTree"] = Record.CandidateTree;
			return Value;
		}

		std::expected<void, AssetExportError> WriteText(const std::filesystem::path& Path, std::string_view Text);

		std::expected<void, AssetExportError> SaveRecoveryRecord(
			const std::filesystem::path& JournalPath, const ExportRecoveryRecord& Record)
		{
			const std::string Data = SerializeRecoveryRecord(Record).dump(2) + "\n";
			if (Data.size() > MaximumRecoveryBytes)
				return std::unexpected(MakeError(AssetExportErrorCode::RecoveryRequired,
					"Export recovery record exceeds its bounded size", JournalPath));
			const auto Temporary = JournalPath.parent_path() /
				std::filesystem::u8path(PathUtf8(JournalPath.filename()) + ".tmp-" + Record.OperationID);
			if (auto Written = WriteText(Temporary, Data); !Written)
				return std::unexpected(MakeError(AssetExportErrorCode::RecoveryRequired,
					"Could not write export recovery record: " + Written.error().Message, Temporary));
			if (auto Replaced = Platform::ReplaceFileAtomically(Temporary, JournalPath); !Replaced)
			{
				std::error_code Error;
				std::filesystem::remove(Temporary, Error);
				return std::unexpected(MakeError(AssetExportErrorCode::RecoveryRequired,
					"Could not atomically publish export recovery record: " + Replaced.error().Message, JournalPath));
			}
			return {};
		}

		std::expected<ExportRecoveryRecord, AssetExportError> LoadRecoveryRecord(
			const std::filesystem::path& JournalPath, const std::filesystem::path& ExpectedOutput)
		{
			auto Bytes = ReadFile(JournalPath, MaximumRecoveryBytes);
			if (!Bytes) return std::unexpected(MakeError(AssetExportErrorCode::RecoveryRequired,
				"Export recovery record cannot be safely read: " + Bytes.error().Message, JournalPath));
			try
			{
				const std::string Text(Bytes->empty() ? "" : reinterpret_cast<const char*>(Bytes->data()), Bytes->size());
				std::vector<std::set<std::string>> ObjectKeys;
				bool DuplicateKey = false;
				const auto Callback = [&ObjectKeys, &DuplicateKey](int, Json::parse_event_t Event, Json& Parsed)
				{
					if (Event == Json::parse_event_t::object_start) ObjectKeys.emplace_back();
					else if (Event == Json::parse_event_t::key && !ObjectKeys.empty())
						DuplicateKey |= !ObjectKeys.back().insert(Parsed.get<std::string>()).second;
					else if (Event == Json::parse_event_t::object_end && !ObjectKeys.empty()) ObjectKeys.pop_back();
					return true;
				};
				const Json Value = Json::parse(Text.begin(), Text.end(), Callback);
				if (DuplicateKey || !Value.is_object())
					return std::unexpected(MakeError(AssetExportErrorCode::RecoveryRequired,
						"Export recovery record is not a unique-key JSON object", JournalPath));
				if (Value.value("format", std::string{}) != RecoveryFormat || Value.value("version", 0u) != RecoveryVersion)
					return std::unexpected(MakeError(AssetExportErrorCode::RecoveryRequired,
						"Export recovery record has an unsupported format", JournalPath));
				ExportRecoveryRecord Record;
				Record.OperationID = Value.at("operationId").get<std::string>();
				Record.ProjectID = Value.at("projectId").get<std::string>();
				Record.ProjectRoot = std::filesystem::u8path(Value.at("projectRoot").get<std::string>());
				Record.Output = std::filesystem::u8path(Value.at("output").get<std::string>());
				Record.Candidate = std::filesystem::u8path(Value.at("candidate").get<std::string>());
				Record.Backup = std::filesystem::u8path(Value.at("backup").get<std::string>());
				Record.HadPreviousOutput = Value.at("hadPreviousOutput").get<bool>();
				Record.State = Value.at("state").get<std::string>();
				const auto ReadTree = [&Value, &JournalPath](const char* Name,
					std::map<std::string, std::string>& Destination) -> std::expected<void, AssetExportError>
				{
					const Json& Tree = Value.at(Name);
					if (!Tree.is_object() || Tree.size() > MaximumExportFileCount * 2)
						return std::unexpected(MakeError(AssetExportErrorCode::RecoveryRequired,
							"Export recovery tree has an invalid entry count", JournalPath));
					for (const auto& [Name, Digest] : Tree.items())
					{
						if (Name.size() < 3 || (Name[0] != 'F' && Name[0] != 'D') || Name[1] != ':' ||
							!IsSafeRelative(std::string_view(Name).substr(2)) || !Digest.is_string())
							return std::unexpected(MakeError(AssetExportErrorCode::RecoveryRequired,
								"Export recovery tree contains an invalid entry", JournalPath));
						const std::string DigestValue = Digest.get<std::string>();
						if ((Name[0] == 'D' && DigestValue != "directory") ||
							(Name[0] == 'F' && (DigestValue.size() != 32 ||
							!std::all_of(DigestValue.begin(), DigestValue.end(), [](unsigned char C) { return std::isxdigit(C) != 0; }))))
							return std::unexpected(MakeError(AssetExportErrorCode::RecoveryRequired,
								"Export recovery tree contains an invalid receipt", JournalPath));
						if (!Destination.emplace(Name, DigestValue).second)
							return std::unexpected(MakeError(AssetExportErrorCode::RecoveryRequired,
								"Export recovery tree contains duplicate entries", JournalPath));
					}
					return {};
				};
				if (Value.size() != 12)
					return std::unexpected(MakeError(AssetExportErrorCode::RecoveryRequired,
						"Export recovery record fields are not recognized", JournalPath));
				if (auto Read = ReadTree("previousTree", Record.PreviousTree); !Read) return std::unexpected(Read.error());
				if (auto Read = ReadTree("candidateTree", Record.CandidateTree); !Read) return std::unexpected(Read.error());
				const auto Operation = UUID::Parse(Record.OperationID);
				const auto Project = UUID::Parse(Record.ProjectID);
				if (!Operation || !Project || Operation->IsNil() || Project->IsNil() ||
					!Record.Output.is_absolute() || !Record.ProjectRoot.is_absolute() ||
					Record.Output.lexically_normal() != ExpectedOutput.lexically_normal() ||
					Record.Candidate != Record.Output.parent_path() / (".pf-export-candidate-" + Record.OperationID) ||
					Record.Backup != Record.Output.parent_path() / (".pf-export-backup-" + Record.OperationID) ||
					(Record.State != RecoveryStateStaging && Record.State != RecoveryStatePrepared && Record.State != RecoveryStateOldMoved &&
						Record.State != RecoveryStateNewPublished && Record.State != RecoveryStateCleanupPending))
					return std::unexpected(MakeError(AssetExportErrorCode::RecoveryRequired,
						"Export recovery record paths, identity or state are invalid", JournalPath));
				return Record;
			}
			catch (const std::exception& Exception)
			{
				return std::unexpected(MakeError(AssetExportErrorCode::RecoveryRequired,
					std::string("Export recovery record is malformed: ") + Exception.what(), JournalPath));
			}
		}

		std::expected<void, AssetExportError> WriteText(const std::filesystem::path& Path, std::string_view Text)
		{
			std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
			if (!Output) return std::unexpected(MakeError(AssetExportErrorCode::StageFailed, "Could not create package metadata", Path));
			Output.write(Text.data(), static_cast<std::streamsize>(Text.size()));
			Output.flush();
			if (!Output) return std::unexpected(MakeError(AssetExportErrorCode::StageFailed, "Could not write package metadata", Path));
			return {};
		}

		std::expected<void, AssetExportError> RemoveJournal(const std::filesystem::path& Path)
		{
			std::error_code Error;
			if (!std::filesystem::remove(Path, Error) && Error)
				return std::unexpected(MakeError(AssetExportErrorCode::RecoveryRequired,
					"Could not remove the completed export recovery record: " + Error.message(), Path));
			return {};
		}

		std::expected<void, AssetExportError> RemoveValidatedTree(
			const std::filesystem::path& Root,
			const std::map<std::string, std::string>& Expected,
			bool AllowPreviouslyRemovedEntries)
		{
			std::error_code Error;
			const auto Status = std::filesystem::symlink_status(Root, Error);
			if (Error == std::errc::no_such_file_or_directory) return {};
			if (Error || std::filesystem::is_symlink(Status) || !std::filesystem::is_directory(Status))
				return std::unexpected(MakeError(AssetExportErrorCode::RecoveryRequired,
					"Export recovery path is missing, linked, or not a directory", Root));
			if (auto Safe = CheckNoLinks(Root); !Safe)
				return std::unexpected(MakeError(AssetExportErrorCode::RecoveryRequired, Safe.error().Message, Safe.error().Path));
			auto Actual = CaptureTree(Root);
			if (!Actual) return std::unexpected(MakeError(AssetExportErrorCode::RecoveryRequired,
				"Could not validate export recovery tree: " + Actual.error().Message, Actual.error().Path));
			if (AllowPreviouslyRemovedEntries)
			{
				for (const auto& [Name, Digest] : *Actual)
				{
					const auto It = Expected.find(Name);
					if (It == Expected.end() || It->second != Digest)
						return std::unexpected(MakeError(AssetExportErrorCode::RecoveryRequired,
							"Export recovery tree contains an unowned or independently changed entry", Root / std::filesystem::u8path(Name.substr(2))));
				}
			}
			else if (*Actual != Expected)
				return std::unexpected(MakeError(AssetExportErrorCode::RecoveryRequired,
					"Export recovery tree does not match its recorded content receipts", Root));
			std::filesystem::remove_all(Root, Error);
			if (Error) return std::unexpected(MakeError(AssetExportErrorCode::RecoveryRequired,
				"Could not remove validated export recovery tree: " + Error.message(), Root));
			return {};
		}

		std::expected<void, AssetExportError> RecoverUnderLock(const std::filesystem::path& Output)
		{
			const auto JournalPath = GetRecoveryPath(Output);
			std::error_code Error;
			if (!std::filesystem::exists(JournalPath, Error))
			{
				if (Error) return std::unexpected(MakeError(AssetExportErrorCode::RecoveryRequired,
					"Could not inspect export recovery record: " + Error.message(), JournalPath));
				return {};
			}
			if (auto Safe = CheckNoLinks(JournalPath); !Safe)
				return std::unexpected(MakeError(AssetExportErrorCode::RecoveryRequired, Safe.error().Message, Safe.error().Path));
			auto Loaded = LoadRecoveryRecord(JournalPath, Output);
			if (!Loaded) return std::unexpected(Loaded.error());
			ExportRecoveryRecord Record = std::move(*Loaded);
			if (Record.HadPreviousOutput != !Record.PreviousTree.empty())
				return std::unexpected(MakeError(AssetExportErrorCode::RecoveryRequired,
					"Export recovery record has inconsistent previous-output receipts", JournalPath));
			if (auto Safe = CheckNoLinks(Record.Output); !Safe)
				return std::unexpected(MakeError(AssetExportErrorCode::RecoveryRequired, Safe.error().Message, Safe.error().Path));

			const auto Inspect = [&Error](const std::filesystem::path& Path, bool& Exists,
				std::map<std::string, std::string>& Actual) -> std::expected<void, AssetExportError>
			{
				Exists = std::filesystem::exists(Path, Error);
				if (Error) return std::unexpected(MakeError(AssetExportErrorCode::RecoveryRequired,
					"Could not observe export recovery path: " + Error.message(), Path));
				if (!Exists) return {};
				if (auto Safe = CheckNoLinks(Path); !Safe)
					return std::unexpected(MakeError(AssetExportErrorCode::RecoveryRequired, Safe.error().Message, Safe.error().Path));
				auto Tree = CaptureTree(Path);
				if (!Tree) return std::unexpected(MakeError(AssetExportErrorCode::RecoveryRequired,
					"Could not validate export recovery path: " + Tree.error().Message, Tree.error().Path));
				Actual = std::move(*Tree);
				return {};
			};
			bool OutputExists = false, CandidateExists = false, BackupExists = false;
			std::map<std::string, std::string> OutputTree, CandidateTree, BackupTree;
			if (auto Read = Inspect(Record.Output, OutputExists, OutputTree); !Read) return std::unexpected(Read.error());
			if (auto Read = Inspect(Record.Candidate, CandidateExists, CandidateTree); !Read) return std::unexpected(Read.error());
			if (auto Read = Inspect(Record.Backup, BackupExists, BackupTree); !Read) return std::unexpected(Read.error());
			const bool OutputIsNew = OutputExists && OutputTree == Record.CandidateTree;
			const bool OutputIsOld = Record.HadPreviousOutput && OutputExists && OutputTree == Record.PreviousTree;
			bool BackupIsOwnedRemainder = BackupExists && BackupTree == Record.PreviousTree;
			if (BackupExists && Record.State == RecoveryStateCleanupPending && OutputIsNew)
			{
				BackupIsOwnedRemainder = true;
				for (const auto& [Name, Digest] : BackupTree)
				{
					const auto Expected = Record.PreviousTree.find(Name);
					if (Expected == Record.PreviousTree.end() || Expected->second != Digest)
					{
						BackupIsOwnedRemainder = false;
						break;
					}
				}
			}
			const bool BackupIsOld = Record.HadPreviousOutput && BackupExists && BackupIsOwnedRemainder;
			const bool CandidateIsNew = CandidateExists && CandidateTree == Record.CandidateTree;
			if ((OutputExists && !OutputIsNew && !OutputIsOld) ||
				(BackupExists && !BackupIsOld) || (CandidateExists && !CandidateIsNew))
				return std::unexpected(MakeError(AssetExportErrorCode::RecoveryRequired,
					"An export recovery path differs from the transaction receipts; preserving output and recovery data", JournalPath));

			if (OutputIsNew)
			{
				if (Record.HadPreviousOutput && BackupExists)
				{
					if (auto Saved = SaveRecoveryRecord(JournalPath, Record); !Saved) return std::unexpected(Saved.error());
					Record.State = RecoveryStateCleanupPending;
					if (auto Saved = SaveRecoveryRecord(JournalPath, Record); !Saved) return std::unexpected(Saved.error());
					if (auto Removed = RemoveValidatedTree(Record.Backup, Record.PreviousTree, true); !Removed) return std::unexpected(Removed.error());
				}
				if (CandidateExists)
					if (auto Removed = RemoveValidatedTree(Record.Candidate, Record.CandidateTree, false); !Removed) return std::unexpected(Removed.error());
				return RemoveJournal(JournalPath);
			}

			if (OutputIsOld && !BackupExists)
			{
				if (Record.State == RecoveryStateStaging && !CandidateExists)
					return RemoveJournal(JournalPath);
				if (!CandidateIsNew)
					return std::unexpected(MakeError(AssetExportErrorCode::RecoveryRequired,
						"The old package is usable, but the transaction candidate cannot be validated", JournalPath));
				if (auto Removed = RemoveValidatedTree(Record.Candidate, Record.CandidateTree, false); !Removed) return std::unexpected(Removed.error());
				return RemoveJournal(JournalPath);
			}

			if (!OutputExists && Record.HadPreviousOutput && BackupIsOld)
			{
				if (CandidateExists && !CandidateIsNew)
					return std::unexpected(MakeError(AssetExportErrorCode::RecoveryRequired,
						"The staged export candidate changed independently; preserving it and the prior package", Record.Candidate));
				std::filesystem::rename(Record.Backup, Record.Output, Error);
				if (Error) return std::unexpected(MakeError(AssetExportErrorCode::RecoveryRequired,
					"Could not restore the previous export package: " + Error.message(), Record.Backup));
				if (CandidateExists)
					if (auto Removed = RemoveValidatedTree(Record.Candidate, Record.CandidateTree, false); !Removed) return std::unexpected(Removed.error());
				return RemoveJournal(JournalPath);
			}

			if (!Record.HadPreviousOutput && !OutputExists && !BackupExists)
			{
				if (CandidateExists)
					if (auto Removed = RemoveValidatedTree(Record.Candidate, Record.CandidateTree, false); !Removed) return std::unexpected(Removed.error());
				return RemoveJournal(JournalPath);
			}

			return std::unexpected(MakeError(AssetExportErrorCode::RecoveryRequired,
				"Export publication state is ambiguous; preserving all package and recovery paths", JournalPath));
		}

		std::string HostPlatform()
		{
#if defined(_WIN32)
			return "Windows";
#elif defined(__APPLE__)
			return "macOS";
#elif defined(__linux__)
			return "Linux";
#else
			return "Unsupported";
#endif
		}

		std::string TargetPlatform(ExportTargetPlatform Value)
		{
		switch (Value)
		{
		case ExportTargetPlatform::Windows: return "Windows";
		case ExportTargetPlatform::Linux: return "Linux";
		case ExportTargetPlatform::MacOS: return "macOS";
		}
		return {};
		}

		std::expected<void, AssetExportError> ValidateProfile(const ExportBuildProfile& Profile)
		{
		auto Serialized = ExportBuildProfileSerializer::Serialize(Profile);
		if (!Serialized) return std::unexpected(MakeError(AssetExportErrorCode::InvalidProfile, Serialized.error().Message));
		if (TargetPlatform(Profile.TargetPlatform) != HostPlatform())
			return std::unexpected(MakeError(AssetExportErrorCode::UnsupportedTarget,
				"Only native host-to-target export is currently supported"));
		return {};
		}

		std::expected<void, AssetExportError> ValidateNativeEntrypoint(
			const std::filesystem::path& Path, const ExportTemplateDescriptor& Descriptor)
		{
			std::ifstream Input(Path, std::ios::binary);
			std::array<unsigned char, 64> Header{};
			Input.read(reinterpret_cast<char*>(Header.data()), static_cast<std::streamsize>(Header.size()));
			if (Input.gcount() < 20)
				return std::unexpected(MakeError(AssetExportErrorCode::TemplateIncomplete, "Template entrypoint header is truncated", Path));
#if !defined(_WIN32)
			std::error_code PermissionError;
			const auto Permissions = std::filesystem::status(Path, PermissionError).permissions();
			const auto ExecuteBits = std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec |
				std::filesystem::perms::others_exec;
			if (PermissionError || (Permissions & ExecuteBits) == std::filesystem::perms::none)
				return std::unexpected(MakeError(AssetExportErrorCode::TemplateIncomplete,
					"Template entrypoint does not have executable permissions on this platform", Path));
#endif
			const auto U16 = [&Header](size_t Offset)
			{
				return static_cast<uint16_t>(Header[Offset]) | static_cast<uint16_t>(Header[Offset + 1] << 8);
			};
			const auto U32 = [&Header](size_t Offset)
			{
				return static_cast<uint32_t>(Header[Offset]) | (static_cast<uint32_t>(Header[Offset + 1]) << 8) |
					(static_cast<uint32_t>(Header[Offset + 2]) << 16) | (static_cast<uint32_t>(Header[Offset + 3]) << 24);
			};
			bool Valid = false;
			switch (Descriptor.TargetPlatform)
			{
			case ExportTargetPlatform::Windows:
				if (Header[0] == 'M' && Header[1] == 'Z')
				{
					const uint32_t PeOffset = U32(0x3c);
					Input.clear();
					Input.seekg(PeOffset, std::ios::beg);
					std::array<unsigned char, 6> PE{};
					Input.read(reinterpret_cast<char*>(PE.data()), static_cast<std::streamsize>(PE.size()));
					Valid = Input.gcount() == static_cast<std::streamsize>(PE.size()) && PE[0] == 'P' && PE[1] == 'E' &&
						PE[2] == 0 && PE[3] == 0 && PE[4] == 0x64 && PE[5] == 0x86 && Descriptor.TargetArchitecture == ExportTargetArchitecture::X64;
				}
				break;
			case ExportTargetPlatform::Linux:
				Valid = Header[0] == 0x7f && Header[1] == 'E' && Header[2] == 'L' && Header[3] == 'F' &&
					Header[4] == 2 && Header[5] == 1 && U16(18) == 62 && Descriptor.TargetArchitecture == ExportTargetArchitecture::X64;
				break;
			case ExportTargetPlatform::MacOS:
				Valid = Header[0] == 0xcf && Header[1] == 0xfa && Header[2] == 0xed && Header[3] == 0xfe &&
					U32(4) == 0x0100000c && Descriptor.TargetArchitecture == ExportTargetArchitecture::Arm64;
				break;
			}
			if (!Valid) return std::unexpected(MakeError(AssetExportErrorCode::TemplateIncompatible,
				"Template executable format or architecture does not match its descriptor", Path));
			return {};
		}

		bool IsOwnedCookedProductPath(std::string_view Name, std::string_view ResourcePrefix)
		{
			const std::string Prefix = std::string(ResourcePrefix) + "Content/";
			if (!Name.starts_with(Prefix)) return false;
			const std::string_view Relative = Name.substr(Prefix.size());
			if (Relative == "manifest.pfmanifest") return true;
			constexpr std::string_view ProductRoot = "g/";
			if (!Relative.starts_with(ProductRoot)) return false;
			const size_t DirectoryEnd = Relative.find('/', ProductRoot.size());
			if (DirectoryEnd == std::string_view::npos) return false;
			const std::string_view Directory = Relative.substr(ProductRoot.size(), DirectoryEnd - ProductRoot.size());
			if (Directory.size() != 32 || !std::all_of(Directory.begin(), Directory.end(), [](unsigned char C)
				{ return std::isxdigit(C) != 0 && !std::isupper(C); })) return false;
			const std::string_view File = Relative.substr(DirectoryEnd + 1);
			if (File.size() == 36 && File.ends_with(".bin"))
				return std::all_of(File.begin(), File.begin() + 32, [](unsigned char C)
					{ return (C >= '0' && C <= '9') || (C >= 'a' && C <= 'f'); });
			// Continue to recognize version-1 cook product locators already recorded in package inventories.
			if (File.size() < 42 || !File.ends_with(".bin")) return false;
			const size_t Separator = File.find('_');
			if (Separator != 36) return false;
			auto ID = UUID::Parse(File.substr(0, Separator));
			if (!ID || ID->IsNil()) return false;
			const std::string_view Version = File.substr(Separator + 1, File.size() - Separator - 5);
			return !Version.empty() && std::all_of(Version.begin(), Version.end(), [](unsigned char C)
				{ return C >= '0' && C <= '9'; });
		}

		bool IsRecognizedPackagePath(std::string_view Name, const std::set<std::string>& TemplateFiles)
		{
			if (TemplateFiles.contains(std::string(Name))) return true;
			if (Name == RuntimeConfigurationName || Name == "Contents/Resources/" + std::string(RuntimeConfigurationName)) return true;
			if (IsOwnedCookedProductPath(Name, "")) return true;
			return IsOwnedCookedProductPath(Name, "Contents/Resources/");
		}

		std::expected<std::set<std::string>, AssetExportError> ValidateInventory(
			const std::filesystem::path& Existing, const std::set<std::string>& TemplateFiles,
			std::string_view ExpectedProfileID, std::string_view ExpectedProjectID)
		{
			const auto InventoryPath = Existing / InventoryName;
			std::error_code Error;
			if (!std::filesystem::exists(InventoryPath, Error))
			{
				if (Error) return std::unexpected(MakeError(AssetExportErrorCode::PublicationFailed,
					"Could not inspect existing export ownership", InventoryPath));
				return std::unexpected(MakeError(AssetExportErrorCode::PublicationFailed,
					"Destination exists without a PulseForge export inventory; it will not be overwritten", Existing));
			}
			auto Tree = EnumerateFiles(Existing);
			if (!Tree) return std::unexpected(Tree.error());
			auto Bytes = ReadFile(InventoryPath, 4u * 1024u * 1024u);
			if (!Bytes) return std::unexpected(Bytes.error());
			try
			{
				const std::string Text(Bytes->empty() ? "" : reinterpret_cast<const char*>(Bytes->data()), Bytes->size());
				std::vector<std::set<std::string>> ObjectKeys;
				bool DuplicateKey = false;
				const auto Callback = [&ObjectKeys, &DuplicateKey](int, Json::parse_event_t Event, Json& Parsed)
				{
					if (Event == Json::parse_event_t::object_start) ObjectKeys.emplace_back();
					else if (Event == Json::parse_event_t::key && !ObjectKeys.empty())
						DuplicateKey |= !ObjectKeys.back().insert(Parsed.get<std::string>()).second;
					else if (Event == Json::parse_event_t::object_end && !ObjectKeys.empty()) ObjectKeys.pop_back();
					return true;
				};
				const Json Inventory = Json::parse(Text.begin(), Text.end(), Callback);
				if (DuplicateKey || !Inventory.is_object() || Inventory.size() != 6 ||
					Inventory.value("format", std::string{}) != "PulseForgeExportInventory" ||
					Inventory.value("version", 0u) != 2u ||
					Inventory.value("profileId", std::string{}) != ExpectedProfileID ||
					Inventory.value("projectId", std::string{}) != ExpectedProjectID ||
					Inventory.value("cookGeneration", std::string{}).empty() ||
					!Inventory.contains("files") || !Inventory["files"].is_object())
					return std::unexpected(MakeError(AssetExportErrorCode::PublicationFailed, "Existing export inventory is invalid", InventoryPath));
				std::set<std::string> Owned;
				for (const auto& [Name, Expected] : Inventory["files"].items())
				{
					if (!IsSafeRelative(Name) || !Expected.is_string() || !IsRecognizedPackagePath(Name, TemplateFiles))
						return std::unexpected(MakeError(AssetExportErrorCode::PublicationFailed, "Existing export inventory entry is invalid", InventoryPath));
					Owned.insert(Name);
					const std::string ExpectedDigest = Expected.get<std::string>();
					if (ExpectedDigest.size() != 32 || !std::all_of(ExpectedDigest.begin(), ExpectedDigest.end(),
						[](unsigned char C) { return (C >= '0' && C <= '9') || (C >= 'a' && C <= 'f'); }))
						return std::unexpected(MakeError(AssetExportErrorCode::PublicationFailed,
							"Existing export inventory digest is invalid", InventoryPath));
					auto Actual = Fingerprint(Existing / std::filesystem::u8path(Name));
					if (!Actual || Actual->DigestHex != ExpectedDigest)
						return std::unexpected(MakeError(AssetExportErrorCode::PublicationFailed,
							"An owned package file changed independently; preserving existing output", Existing / std::filesystem::u8path(Name)));
				}
				return Owned;
			}
			catch (const std::exception& Exception)
			{
				return std::unexpected(MakeError(AssetExportErrorCode::PublicationFailed,
					std::string("Existing export inventory cannot be parsed: ") + Exception.what(), InventoryPath));
			}
		}
	} // namespace

	std::expected<std::string, AssetExportError> SerializeExportTemplateDescriptor(const ExportTemplateDescriptor& Descriptor)
	{
		if (Descriptor.FormatVersion != ExportTemplateFormatVersion || Descriptor.RuntimeABI != CookedRuntimeABIIdentifier ||
			Descriptor.RendererBackend != "Vulkan" || Descriptor.CookManifestSchema != CookedAssetCatalog::ManifestVersion ||
			Descriptor.Entrypoint.empty() || !IsSafeRelative(Descriptor.Entrypoint) || Descriptor.RuntimeFiles.empty() ||
			Descriptor.RuntimeFiles.size() > MaximumExportFileCount)
			return std::unexpected(MakeError(AssetExportErrorCode::TemplateIncompatible, "Template descriptor fields are incomplete or unsupported"));
		std::set<std::string> Unique;
		for (const std::string& File : Descriptor.RuntimeFiles)
			if (!IsSafeRelative(File) || !Unique.insert(File).second)
				return std::unexpected(MakeError(AssetExportErrorCode::TemplateIncompatible, "Template runtime files must be unique safe relative paths"));
		if (!Unique.contains(Descriptor.Entrypoint))
			return std::unexpected(MakeError(AssetExportErrorCode::TemplateIncompatible, "Template file list must include its entrypoint"));
		Json Document;
		Document["format"] = ExportTemplateFormat;
		Document["version"] = Descriptor.FormatVersion;
		Document["targetPlatform"] = TargetPlatform(Descriptor.TargetPlatform);
		Document["architecture"] = Descriptor.TargetArchitecture == ExportTargetArchitecture::X64 ? "x64" : "arm64";
		Document["configuration"] = GetCookedBuildConfigurationName(Descriptor.Configuration);
		Document["renderer"] = Descriptor.RendererBackend;
		Document["runtimeABI"] = Descriptor.RuntimeABI;
		Document["cookManifestSchema"] = Descriptor.CookManifestSchema;
		Document["entrypoint"] = Descriptor.Entrypoint;
		Document["runtimeFiles"] = Descriptor.RuntimeFiles;
		return Document.dump(2) + "\n";
	}

	std::expected<ExportTemplateDescriptor, AssetExportError> LoadExportTemplateDescriptor(const std::filesystem::path& Path)
	{
		auto Bytes = ReadFile(Path, MaximumDescriptorBytes);
		if (!Bytes) return std::unexpected(MakeError(AssetExportErrorCode::TemplateMissing, "Could not read template descriptor", Path));
		try
		{
			const std::string Text(Bytes->empty() ? "" : reinterpret_cast<const char*>(Bytes->data()), Bytes->size());
			const Json Document = Json::parse(Text);
			if (Document.value("format", std::string{}) != ExportTemplateFormat ||
				Document.value("version", 0u) != ExportTemplateFormatVersion)
				return std::unexpected(MakeError(AssetExportErrorCode::TemplateIncompatible,
					"Template format or version is unsupported", Path));
			ExportTemplateDescriptor Result;
			const std::string Platform = Document.at("targetPlatform").get<std::string>();
			if (Platform == "Windows") Result.TargetPlatform = ExportTargetPlatform::Windows;
			else if (Platform == "Linux") Result.TargetPlatform = ExportTargetPlatform::Linux;
			else if (Platform == "macOS") Result.TargetPlatform = ExportTargetPlatform::MacOS;
			else return std::unexpected(MakeError(AssetExportErrorCode::TemplateIncompatible, "Template target platform is invalid", Path));
			const std::string Architecture = Document.at("architecture").get<std::string>();
			if (Architecture == "x64") Result.TargetArchitecture = ExportTargetArchitecture::X64;
			else if (Architecture == "arm64") Result.TargetArchitecture = ExportTargetArchitecture::Arm64;
			else return std::unexpected(MakeError(AssetExportErrorCode::TemplateIncompatible, "Template architecture is invalid", Path));
			auto Configuration = ParseCookedBuildConfiguration(Document.at("configuration").get<std::string>());
			if (!Configuration) return std::unexpected(MakeError(AssetExportErrorCode::TemplateIncompatible, Configuration.error(), Path));
			Result.Configuration = *Configuration;
			Result.RendererBackend = Document.at("renderer").get<std::string>();
			Result.RuntimeABI = Document.at("runtimeABI").get<std::string>();
			Result.CookManifestSchema = Document.at("cookManifestSchema").get<uint32_t>();
			Result.Entrypoint = Document.at("entrypoint").get<std::string>();
			Result.RuntimeFiles = Document.at("runtimeFiles").get<std::vector<std::string>>();
			auto Valid = SerializeExportTemplateDescriptor(Result);
			if (!Valid) return std::unexpected(Valid.error());
			return Result;
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(MakeError(AssetExportErrorCode::TemplateIncompatible,
				std::string("Template descriptor is malformed: ") + Exception.what(), Path));
		}
	}

	AssetExportService::~AssetExportService() { Shutdown(); }

	std::expected<void, AssetExportError> AssetExportService::RecoverInterruptedPublication(
		const std::filesystem::path& OutputPath)
	{
		auto Output = Absolute(OutputPath);
		if (!Output || Output->filename().empty())
			return std::unexpected(Output ? MakeError(AssetExportErrorCode::InvalidRequest,
				"Recovery output must name a package directory", OutputPath) : Output.error());
		if (auto Safe = CheckNoLinks(*Output); !Safe) return std::unexpected(Safe.error());
		const auto JournalPath = GetRecoveryPath(*Output);
		std::error_code Error;
		if (!std::filesystem::exists(JournalPath, Error))
		{
			if (Error) return std::unexpected(MakeError(AssetExportErrorCode::RecoveryRequired,
				"Could not inspect export recovery record: " + Error.message(), JournalPath));
			return {};
		}
		const auto LockPath = Output->parent_path() / ("." + PathUtf8(Output->filename()) + ".pf-export-lock");
		if (auto Safe = CheckNoLinks(LockPath); !Safe) return std::unexpected(Safe.error());
		ExportProcessLock Lock(LockPath);
		if (!Lock.Acquired()) return std::unexpected(MakeError(AssetExportErrorCode::Busy,
			"An export process currently owns this destination", LockPath));
		return RecoverUnderLock(*Output);
	}

	std::expected<uint64_t, AssetExportError> AssetExportService::Start(AssetExportRequest Request)
	{
		std::lock_guard Lock(m_Mutex);
		if (m_ShuttingDown) return std::unexpected(MakeError(AssetExportErrorCode::Cancelled, "Export service is shutting down"));
		if (m_Snapshot.Running) return std::unexpected(MakeError(AssetExportErrorCode::Busy, "An export is already running"));
		if (m_Worker.joinable())
		{
			// A prior worker is terminal. Joining here is bounded to its already completed function.
			m_Worker.join();
		}
		const uint64_t ID = m_NextRequestID++;
		m_CancelRequested.store(false, std::memory_order_release);
		m_Snapshot = {};
		m_Snapshot.RequestID = ID;
		m_Snapshot.Running = true;
		m_Snapshot.Phase = AssetExportPhase::Validate;
		m_Worker = std::jthread([this, ID, Request = std::move(Request)]() mutable { Run(ID, std::move(Request)); });
		return ID;
	}

	AssetExportSnapshot AssetExportService::GetSnapshot(uint64_t RequestID) const
	{
		std::lock_guard Lock(m_Mutex);
		return m_Snapshot.RequestID == RequestID ? m_Snapshot : AssetExportSnapshot{};
	}

	bool AssetExportService::RequestCancel(uint64_t RequestID) noexcept
	{
		std::lock_guard Lock(m_Mutex);
		if (m_Snapshot.RequestID != RequestID || !m_Snapshot.Running) return false;
		m_CancelRequested.store(true, std::memory_order_release);
		m_Snapshot.CancellationRequested = true;
		return true;
	}

	void AssetExportService::Shutdown() noexcept
	{
		{
			std::lock_guard Lock(m_Mutex);
			if (m_ShuttingDown && !m_Worker.joinable()) return;
			m_ShuttingDown = true;
			m_CancelRequested.store(true, std::memory_order_release);
		}
		if (m_Worker.joinable()) m_Worker.join();
	}

	void AssetExportService::Update(uint64_t RequestID, AssetExportPhase Phase, float Progress, std::string Message)
	{
		std::lock_guard Lock(m_Mutex);
		if (m_Snapshot.RequestID != RequestID) return;
		m_Snapshot.Phase = Phase;
		m_Snapshot.Progress = std::clamp(Progress, 0.0f, 1.0f);
		if (!Message.empty())
		{
			if (Message.size() > MaximumExportLogBytes) Message.erase(0, Message.size() - MaximumExportLogBytes);
			if (m_Snapshot.Log.size() + Message.size() > MaximumExportLogBytes)
			{
				const size_t Remove = m_Snapshot.Log.size() + Message.size() - MaximumExportLogBytes;
				m_Snapshot.Log.erase(0, Remove);
				m_Snapshot.LogTruncated = true;
			}
			m_Snapshot.Log += Message;
			m_Snapshot.Log.push_back('\n');
		}
	}

	void AssetExportService::Finish(uint64_t RequestID, AssetExportPhase Phase,
		std::optional<AssetExportError> Error, std::filesystem::path Output) noexcept
	{
		std::lock_guard Lock(m_Mutex);
		if (m_Snapshot.RequestID != RequestID) return;
		m_Snapshot.Phase = Phase;
		m_Snapshot.Progress = Phase == AssetExportPhase::Complete ? 1.0f : m_Snapshot.Progress;
		m_Snapshot.Running = false;
		m_Snapshot.Error = std::move(Error);
		m_Snapshot.PublishedOutput = std::move(Output);
	}

	void AssetExportService::Run(uint64_t RequestID, AssetExportRequest Request) noexcept
	{
		const auto Cancelled = [this]() { return m_CancelRequested.load(std::memory_order_acquire); };
		try
		{
			auto ValidProfile = ValidateProfile(Request.Profile);
			if (!ValidProfile) { Finish(RequestID, AssetExportPhase::Failed, ValidProfile.error()); return; }
			if (Request.SourceProjectFile.empty() || Request.TemplateRoot.empty() || Request.TemplateDescriptor.empty())
				{ Finish(RequestID, AssetExportPhase::Failed, MakeError(AssetExportErrorCode::InvalidRequest, "Project, template root and descriptor are required")); return; }
			if (Cancelled()) { Finish(RequestID, AssetExportPhase::Cancelled, MakeError(AssetExportErrorCode::Cancelled, "Export cancelled before template resolution")); return; }
			Update(RequestID, AssetExportPhase::ResolveTemplate, 0.08f, "Validating native player template");
			auto Descriptor = LoadExportTemplateDescriptor(Request.TemplateDescriptor);
			if (!Descriptor) { Finish(RequestID, AssetExportPhase::Failed, Descriptor.error()); return; }
			if (Descriptor->TargetPlatform != Request.Profile.TargetPlatform || Descriptor->TargetArchitecture != Request.Profile.Architecture ||
				Descriptor->Configuration != Request.Profile.Configuration || Descriptor->RendererBackend != "Vulkan")
				{ Finish(RequestID, AssetExportPhase::Failed, MakeError(AssetExportErrorCode::TemplateIncompatible, "Template target, architecture, configuration, or renderer differs from profile", Request.TemplateDescriptor)); return; }
			if (Descriptor->RuntimeABI != CookedRuntimeABIIdentifier || Descriptor->CookManifestSchema != CookedAssetCatalog::ManifestVersion)
				{ Finish(RequestID, AssetExportPhase::Failed, MakeError(AssetExportErrorCode::TemplateIncompatible, "Template ABI or cooked manifest schema is incompatible", Request.TemplateDescriptor)); return; }
			Update(RequestID, AssetExportPhase::ResolveTemplate, 0.12f, "Template descriptor compatibility verified");
			const auto TemplateEntrypoint = Request.TemplateRoot / std::filesystem::u8path(Descriptor->Entrypoint);
			if (auto NoLinks = CheckNoLinks(TemplateEntrypoint); !NoLinks)
				{ Finish(RequestID, AssetExportPhase::Failed, NoLinks.error()); return; }
			if (auto Entrypoint = ValidateNativeEntrypoint(TemplateEntrypoint, *Descriptor); !Entrypoint)
				{ Finish(RequestID, AssetExportPhase::Failed, Entrypoint.error()); return; }
			Update(RequestID, AssetExportPhase::ResolveTemplate, 0.14f, "Template executable format verified");
			auto Project = Project::Open(Request.SourceProjectFile);
			if (!Project) { Finish(RequestID, AssetExportPhase::Failed, MakeError(AssetExportErrorCode::ProjectOpenFailed, Project.error().Message, Request.SourceProjectFile)); return; }
			Update(RequestID, AssetExportPhase::Validate, 0.18f, "Source project opened and identity captured");
			auto SourceRoot = Absolute(Project->GetRootPath());
			if (!SourceRoot) { Finish(RequestID, AssetExportPhase::Failed, SourceRoot.error()); return; }
			std::filesystem::path Output;
			if (Request.OutputOverride)
				Output = *Request.OutputOverride;
			else
			{
				auto UserData = Platform::GetUserDataDirectory();
				if (!UserData) { Finish(RequestID, AssetExportPhase::Failed, MakeError(AssetExportErrorCode::InvalidRequest,
					"A writable user-data directory is required when no output override is supplied: " + UserData.error().Message)); return; }
				Output = *UserData / "PulseForge" / "Exports" /
					Project->GetDescription().ID.ToString() / Request.Profile.ProfileID.ToString() /
					Request.Profile.DefaultOutputPath;
				if (Request.Profile.TargetPlatform == ExportTargetPlatform::MacOS)
					Output /= std::filesystem::u8path(Request.Profile.ProfileID.ToString() + ".app");
			}
			auto OutputAbs = Absolute(Output);
			if (!OutputAbs) { Finish(RequestID, AssetExportPhase::Failed, OutputAbs.error()); return; }
			Output = *OutputAbs;
			auto Template = Absolute(Request.TemplateRoot);
			if (!Template) { Finish(RequestID, AssetExportPhase::Failed, Template.error()); return; }
			if (Contains(*SourceRoot, Output) || Contains(Output, *SourceRoot) || Contains(*Template, Output) || Contains(Output, *Template))
				{ Finish(RequestID, AssetExportPhase::Failed, MakeError(AssetExportErrorCode::InvalidRequest, "Export destination may not overlap project or template inputs", Output)); return; }
			Update(RequestID, AssetExportPhase::Validate, 0.20f, "Destination and source/template separation verified");
			auto OutputLinks = CheckNoLinks(Output);
			if (!OutputLinks) { Finish(RequestID, AssetExportPhase::Failed, OutputLinks.error()); return; }
			std::error_code Error;
			std::set<std::string> PreviousOwned;
			std::map<std::string, std::string> ExistingTree;
			const std::set<std::string> TemplateFiles(Descriptor->RuntimeFiles.begin(), Descriptor->RuntimeFiles.end());
			std::filesystem::create_directories(Output.parent_path(), Error);
			if (Error) { Finish(RequestID, AssetExportPhase::Failed, MakeError(AssetExportErrorCode::StageFailed, "Could not create export parent: " + Error.message(), Output.parent_path())); return; }
			const std::filesystem::path LockPath = Output.parent_path() / ("." + PathUtf8(Output.filename()) + ".pf-export-lock");
			if (auto Safe = CheckNoLinks(LockPath); !Safe)
				{ Finish(RequestID, AssetExportPhase::Failed, Safe.error()); return; }
			ExportProcessLock Lock(LockPath);
			if (!Lock.Acquired())
				{ Finish(RequestID, AssetExportPhase::Failed, MakeError(AssetExportErrorCode::Busy,
					"Another export process currently owns this destination", LockPath)); return; }
			if (auto Recovered = RecoverUnderLock(Output); !Recovered)
				{ Finish(RequestID, AssetExportPhase::Failed, Recovered.error()); return; }
			if (std::filesystem::exists(Output, Error))
			{
				if (Error || !std::filesystem::is_directory(Output)) { Finish(RequestID, AssetExportPhase::Failed, MakeError(AssetExportErrorCode::PublicationFailed, "Export destination is not a directory", Output)); return; }
				auto Existing = ValidateInventory(Output, TemplateFiles, Request.Profile.ProfileID.ToString(),
					Project->GetDescription().ID.ToString());
				if (!Existing) { Finish(RequestID, AssetExportPhase::Failed, Existing.error()); return; }
				PreviousOwned = std::move(*Existing);
				auto Tree = CaptureTree(Output);
				if (!Tree) { Finish(RequestID, AssetExportPhase::Failed, Tree.error()); return; }
				ExistingTree = std::move(*Tree);
			}
			auto Operation = UUID::Generate();
			if (!Operation) { Finish(RequestID, AssetExportPhase::Failed, MakeError(AssetExportErrorCode::InternalFailure, Operation.error().Message)); return; }
			const auto Candidate = Output.parent_path() / (".pf-export-candidate-" + Operation->ToString());
			const auto Backup = Output.parent_path() / (".pf-export-backup-" + Operation->ToString());
			const auto JournalPath = GetRecoveryPath(Output);
			ExportRecoveryRecord Recovery;
			Recovery.OperationID = Operation->ToString();
			Recovery.ProjectID = Project->GetDescription().ID.ToString();
			Recovery.ProjectRoot = *SourceRoot;
			Recovery.Output = Output;
			Recovery.Candidate = Candidate;
			Recovery.Backup = Backup;
			Recovery.HadPreviousOutput = std::filesystem::exists(Output);
			Recovery.State = RecoveryStateStaging;
			Recovery.PreviousTree = ExistingTree;
			if (auto Safe = CheckNoLinks(JournalPath); !Safe)
				{ Finish(RequestID, AssetExportPhase::Failed, Safe.error()); return; }
			if (auto Saved = SaveRecoveryRecord(JournalPath, Recovery); !Saved)
				{ Finish(RequestID, AssetExportPhase::Failed, Saved.error()); return; }
			struct RecoveryCleanup final
			{
				std::filesystem::path Path;
				bool Keep = false;
				~RecoveryCleanup()
				{
					if (!Keep) { std::error_code Error; std::filesystem::remove(Path, Error); }
				}
			} JournalCleanup{ JournalPath };
			struct Cleanup final
			{
				std::filesystem::path Path;
				bool Created = false;
				~Cleanup()
				{
					if (!Created) return;
					std::error_code E;
					std::filesystem::remove_all(Path, E);
				}
			} CandidateCleanup{ Candidate };
			std::filesystem::create_directory(Candidate, Error);
			if (Error) { Finish(RequestID, AssetExportPhase::Failed, MakeError(AssetExportErrorCode::StageFailed, "Could not create unique export candidate: " + Error.message(), Candidate)); return; }
			CandidateCleanup.Created = true;
			if (std::filesystem::exists(Output))
			{
				for (const auto& [Entry, Digest] : ExistingTree)
				{
					(void)Digest;
					if (!Entry.starts_with("D:")) continue;
					const auto Destination = Candidate / std::filesystem::u8path(Entry.substr(2));
					std::filesystem::create_directories(Destination, Error);
					if (Error) { Finish(RequestID, AssetExportPhase::Failed, MakeError(AssetExportErrorCode::StageFailed,
						"Could not preserve an existing package directory: " + Error.message(), Destination)); return; }
				}
				for (const auto& [Entry, ExpectedDigest] : ExistingTree)
				{
					if (Entry.starts_with("D:")) continue;
					const std::string Name = Entry.substr(2);
					const auto Source = Output / std::filesystem::u8path(Name);
					if (auto Safe = CheckNoLinks(Source); !Safe) { Finish(RequestID, AssetExportPhase::Failed, Safe.error()); return; }
					auto Actual = Fingerprint(Source);
					if (!Actual || Actual->DigestHex != ExpectedDigest)
						{ Finish(RequestID, AssetExportPhase::Failed, MakeError(AssetExportErrorCode::PublicationFailed,
							"An existing package file changed while the candidate was being staged", Source)); return; }
					if (auto Copied = CopyOne(Source, Candidate / std::filesystem::u8path(Name)); !Copied)
						{ Finish(RequestID, AssetExportPhase::Failed, Copied.error()); return; }
				}
			}
			Update(RequestID, AssetExportPhase::Stage, 0.14f, "Staging validated runtime template");
			for (const std::string& Relative : Descriptor->RuntimeFiles)
			{
				if (Cancelled()) { Finish(RequestID, AssetExportPhase::Cancelled, MakeError(AssetExportErrorCode::Cancelled, "Export cancelled during template staging")); return; }
				const auto Source = *Template / std::filesystem::u8path(Relative);
				if (auto Safe = CheckNoLinks(Source); !Safe) { Finish(RequestID, AssetExportPhase::Failed, Safe.error()); return; }
				const auto Status = std::filesystem::symlink_status(Source, Error);
				if (Error || !std::filesystem::is_regular_file(Status)) { Finish(RequestID, AssetExportPhase::Failed, MakeError(AssetExportErrorCode::TemplateIncomplete, "Required template file is missing or not regular", Source)); return; }
				if (std::filesystem::file_size(Source, Error) > MaximumExportFileBytes || Error) { Finish(RequestID, AssetExportPhase::Failed, MakeError(AssetExportErrorCode::TemplateIncomplete, "Template file exceeds the bounded size", Source)); return; }
				if (auto Copied = CopyOne(Source, Candidate / std::filesystem::u8path(Relative)); !Copied) { Finish(RequestID, AssetExportPhase::Failed, Copied.error()); return; }
			}
			if (Cancelled()) { Finish(RequestID, AssetExportPhase::Cancelled, MakeError(AssetExportErrorCode::Cancelled, "Export cancelled before cooking")); return; }
			Update(RequestID, AssetExportPhase::Cook, 0.25f, "Resolving and cooking the saved project content");
			auto Snapshot = AssetDependencyCatalogBuilder::Build(Project->GetRootPath(), Project->GetDescription().ID, 1);
			if (!Snapshot) { Finish(RequestID, AssetExportPhase::Failed, MakeError(AssetExportErrorCode::DependencyResolutionFailed, Snapshot.error().Message)); return; }
			AssetCookRequest CookRequest;
			CookRequest.SourceSnapshot = std::move(*Snapshot);
			CookRequest.SourceProjectFile = Project->GetFilePath();
			CookRequest.StartupScene = Request.Profile.StartupScene;
			CookRequest.DynamicRoots = Request.Profile.DynamicRoots;
			CookRequest.Compatibility = { TargetPlatform(Request.Profile.TargetPlatform), "Vulkan", std::string(CookedRuntimeABIIdentifier), Request.Profile.Configuration };
			const std::filesystem::path ResourceRelative = Request.Profile.TargetPlatform == ExportTargetPlatform::MacOS
				? std::filesystem::path("Contents") / "Resources" : std::filesystem::path{};
			const std::filesystem::path ResourceRoot = Candidate / ResourceRelative;
			CookRequest.OutputRoot = ResourceRoot / "Content";
			auto Cooked = AssetCookService::Cook(CookRequest);
			if (!Cooked) { Finish(RequestID, AssetExportPhase::Failed, MakeError(AssetExportErrorCode::CookFailed, Cooked.error().Message, Cooked.error().Path)); return; }
			std::string RuntimeConfig = "project=" + Project->GetDescription().ID.ToString() + "\n";
			if (Request.Profile.Window)
				RuntimeConfig += "window=" + std::to_string(Request.Profile.Window->Width) + "x" +
					std::to_string(Request.Profile.Window->Height) + "\n";
			if (auto ConfigWrite = WriteText(ResourceRoot / RuntimeConfigurationName, RuntimeConfig); !ConfigWrite)
				{ Finish(RequestID, AssetExportPhase::Failed, ConfigWrite.error()); return; }
			if (Cancelled()) { Finish(RequestID, AssetExportPhase::Cancelled, MakeError(AssetExportErrorCode::Cancelled, "Export cancelled after cooking; previous package remains usable")); return; }
			Update(RequestID, AssetExportPhase::Verify, 0.82f, "Verifying package inventory and cooked manifest");
			auto Files = EnumerateFiles(Candidate);
			if (!Files) { Finish(RequestID, AssetExportPhase::Failed, Files.error()); return; }
			std::set<std::string> CurrentOwned(Descriptor->RuntimeFiles.begin(), Descriptor->RuntimeFiles.end());
			auto CookedFiles = EnumerateFiles(ResourceRoot / "Content");
			if (!CookedFiles) { Finish(RequestID, AssetExportPhase::Failed, CookedFiles.error()); return; }
			for (const std::string& Name : *CookedFiles)
				CurrentOwned.insert((ResourceRelative / "Content" / std::filesystem::u8path(Name)).generic_string());
			CurrentOwned.insert((ResourceRelative / RuntimeConfigurationName).generic_string());
			for (const std::string& Stale : PreviousOwned)
			{
				if (CurrentOwned.contains(Stale)) continue;
				const auto StalePath = Candidate / std::filesystem::u8path(Stale);
				if (auto Safe = CheckNoLinks(StalePath); !Safe) { Finish(RequestID, AssetExportPhase::Failed, Safe.error()); return; }
				const auto Expected = ExistingTree.find("F:" + Stale);
				auto Actual = Fingerprint(StalePath);
				if (Expected == ExistingTree.end() || !Actual || Actual->DigestHex != Expected->second)
					{ Finish(RequestID, AssetExportPhase::Failed, MakeError(AssetExportErrorCode::PublicationFailed,
						"Stale owned file no longer matches the previous package receipt", StalePath)); return; }
				std::filesystem::remove(StalePath, Error);
				if (Error) { Finish(RequestID, AssetExportPhase::Failed, MakeError(AssetExportErrorCode::StageFailed,
					"Could not remove stale owned package file: " + Error.message(), StalePath)); return; }
			}
			Json OwnedFiles = Json::object();
			for (const std::string& Name : CurrentOwned)
			{
				auto Digest = Fingerprint(Candidate / std::filesystem::u8path(Name));
				if (!Digest) { Finish(RequestID, AssetExportPhase::Failed, Digest.error()); return; }
				OwnedFiles[Name] = Digest->DigestHex;
			}
			Json Inventory;
			Inventory["format"] = "PulseForgeExportInventory";
			Inventory["version"] = 2;
			Inventory["profileId"] = Request.Profile.ProfileID.ToString();
			Inventory["projectId"] = Project->GetDescription().ID.ToString();
			Inventory["cookGeneration"] = Cooked->GenerationIdentity;
			Inventory["files"] = std::move(OwnedFiles);
			if (auto Saved = WriteText(Candidate / InventoryName, Inventory.dump(2) + "\n"); !Saved) { Finish(RequestID, AssetExportPhase::Failed, Saved.error()); return; }
			if (Cancelled()) { Finish(RequestID, AssetExportPhase::Cancelled, MakeError(AssetExportErrorCode::Cancelled, "Export cancelled before package publication")); return; }
			Update(RequestID, AssetExportPhase::Publish, 0.94f, "Publishing verified package");
			const bool OutputStillExists = std::filesystem::exists(Output, Error);
			if (Error) { Finish(RequestID, AssetExportPhase::Failed, MakeError(AssetExportErrorCode::PublicationFailed, "Could not revalidate destination: " + Error.message(), Output)); return; }
			if (OutputStillExists)
			{
				auto FreshTree = CaptureTree(Output);
				if (!FreshTree || *FreshTree != ExistingTree)
					{ Finish(RequestID, AssetExportPhase::Failed, MakeError(AssetExportErrorCode::PublicationFailed,
						"Existing package membership or contents changed after staging; preserving it", Output)); return; }
				auto FreshOwnership = ValidateInventory(Output, TemplateFiles, Request.Profile.ProfileID.ToString(),
					Project->GetDescription().ID.ToString());
				if (!FreshOwnership || *FreshOwnership != PreviousOwned)
					{ Finish(RequestID, AssetExportPhase::Failed, FreshOwnership ?
						MakeError(AssetExportErrorCode::PublicationFailed, "Package ownership changed after staging", Output) : FreshOwnership.error()); return; }
			}
			else if (!ExistingTree.empty())
				{ Finish(RequestID, AssetExportPhase::Failed, MakeError(AssetExportErrorCode::PublicationFailed,
					"Existing package disappeared after staging; refusing to replace it", Output)); return; }
			auto CandidateReceipt = CaptureTree(Candidate);
			if (!CandidateReceipt)
				{ Finish(RequestID, AssetExportPhase::Failed, CandidateReceipt.error()); return; }
			Recovery.HadPreviousOutput = OutputStillExists;
			Recovery.State = RecoveryStatePrepared;
			Recovery.PreviousTree = ExistingTree;
			Recovery.CandidateTree = std::move(*CandidateReceipt);
			if (auto Saved = SaveRecoveryRecord(JournalPath, Recovery); !Saved)
				{ Finish(RequestID, AssetExportPhase::Failed, Saved.error()); return; }
			JournalCleanup.Keep = true;
			CandidateCleanup.Path.clear();
			if (std::filesystem::exists(Output))
			{
				std::filesystem::rename(Output, Backup, Error);
				if (Error) { Finish(RequestID, AssetExportPhase::Failed, MakeError(AssetExportErrorCode::RecoveryRequired,
					"Could not retain previous package; recovery record remains: " + Error.message(), Output)); return; }
				Recovery.State = RecoveryStateOldMoved;
				if (auto Saved = SaveRecoveryRecord(JournalPath, Recovery); !Saved)
					{ Finish(RequestID, AssetExportPhase::Failed, Saved.error()); return; }
			}
			std::filesystem::rename(Candidate, Output, Error);
			if (Error)
			{
				Finish(RequestID, AssetExportPhase::Failed, MakeError(AssetExportErrorCode::RecoveryRequired,
					"Could not publish the candidate; a recovery record retains the previous package and candidate: " + Error.message(), Output));
				return;
			}
			Recovery.State = RecoveryStateNewPublished;
			if (auto Saved = SaveRecoveryRecord(JournalPath, Recovery); !Saved)
				{ Finish(RequestID, AssetExportPhase::Failed, Saved.error(), Output); return; }
			if (std::filesystem::exists(Backup))
			{
				Recovery.State = RecoveryStateCleanupPending;
				if (auto Saved = SaveRecoveryRecord(JournalPath, Recovery); !Saved)
					{ Finish(RequestID, AssetExportPhase::Failed, Saved.error(), Output); return; }
				if (auto Removed = RemoveValidatedTree(Backup, Recovery.PreviousTree, false); !Removed)
					{ Finish(RequestID, AssetExportPhase::Failed, Removed.error(), Output); return; }
			}
			if (auto Removed = RemoveJournal(JournalPath); !Removed)
				{ Finish(RequestID, AssetExportPhase::Failed, Removed.error(), Output); return; }
			Update(RequestID, AssetExportPhase::Complete, 1.0f, "Export completed");
			Finish(RequestID, AssetExportPhase::Complete, std::nullopt, Output);
		}
		catch (const std::exception& Exception)
		{
			Finish(RequestID, AssetExportPhase::Failed, MakeError(AssetExportErrorCode::InternalFailure, Exception.what()));
		}
		catch (...)
		{
			Finish(RequestID, AssetExportPhase::Failed, MakeError(AssetExportErrorCode::InternalFailure, "Unknown exception during export"));
		}
	}
} // namespace PulseForge
