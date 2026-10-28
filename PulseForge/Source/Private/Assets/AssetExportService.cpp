#include "Assets/AssetExportService.h"

#include "Assets/AssetDependencyCatalog.h"
#include "Assets/AssetDependencyExtractor.h"
#include "Assets/AssetImportProvenance.h"
#include "Assets/Project.h"
#include "Core/PlatformServices.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <system_error>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace PulseForge
{
	namespace
	{
		using Json = nlohmann::ordered_json;
		constexpr size_t MaximumDescriptorBytes = 1024u * 1024u;
		constexpr std::string_view InventoryName = ".pf-export-inventory.json";
		constexpr std::string_view TemplateOwnershipName = ".pf-export-template-ownership.json";
		constexpr std::string_view RuntimeConfigurationName = "PulseForgeRuntime.cfg";
		constexpr std::string_view CookStagingDirectoryName = ".pf-export-cook-stage";
		constexpr std::string_view RecoveryFormat = "PulseForgeExportRecovery";
		constexpr uint32_t RecoveryVersion = 1;
		constexpr uint64_t MaximumRecoveryBytes = 16u * 1024u * 1024u;
		constexpr std::string_view RecoveryStatePrepared = "Prepared";
		constexpr std::string_view RecoveryStateStaging = "Staging";
		constexpr std::string_view RecoveryStateOldMoved = "OldMoved";
		constexpr std::string_view RecoveryStateNewPublished = "NewPublished";
		constexpr std::string_view RecoveryStateCleanupPending = "CleanupPending";
		constexpr std::string_view RecoveryStateStagingCleanupPending = "StagingCleanupPending";

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

		enum class ExportFileRole : uint8_t
		{
			Template,
			Cooked,
			RuntimeConfiguration,
			TemplateOwnership,
			Inventory
		};

		struct ExportFileDestination final
		{
			std::string Path;
			ExportFileRole Role = ExportFileRole::Template;
		};

		struct ExportInventoryOwnership final
		{
			std::set<std::string> Files;
			std::set<std::string> TemplateFiles;
			std::string TemplateEntrypoint;
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

		bool FilesystemComponentEqual(std::string_view First, std::string_view Second, bool CaseSensitive);

		bool Contains(const std::filesystem::path& Parent, const std::filesystem::path& Child, bool CaseSensitive)
		{
			auto P = Parent.begin();
			auto C = Child.begin();
			for (; P != Parent.end(); ++P, ++C)
				if (C == Child.end() || !FilesystemComponentEqual(PathUtf8(*P), PathUtf8(*C), CaseSensitive)) return false;
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

		std::vector<std::string_view> SplitRelativePath(std::string_view Value)
		{
			std::vector<std::string_view> Parts;
			size_t Begin = 0;
			while (Begin < Value.size())
			{
				const size_t End = Value.find('/', Begin);
				Parts.push_back(Value.substr(Begin, End == std::string_view::npos ? Value.size() - Begin : End - Begin));
				if (End == std::string_view::npos) break;
				Begin = End + 1;
			}
			return Parts;
		}

		bool FilesystemComponentEqual(std::string_view First, std::string_view Second, bool CaseSensitive)
		{
			if (First == Second || CaseSensitive) return First == Second;
			if (First.size() != Second.size()) return false;
			for (size_t Index = 0; Index < First.size(); ++Index)
			{
				unsigned char FirstByte = static_cast<unsigned char>(First[Index]);
				unsigned char SecondByte = static_cast<unsigned char>(Second[Index]);
				if (FirstByte >= 'A' && FirstByte <= 'Z') FirstByte = static_cast<unsigned char>(FirstByte - 'A' + 'a');
				if (SecondByte >= 'A' && SecondByte <= 'Z') SecondByte = static_cast<unsigned char>(SecondByte - 'A' + 'a');
				if (FirstByte != SecondByte) return false;
			}
			return true;
		}

		bool FilesystemPathsEqual(std::string_view First, std::string_view Second, bool CaseSensitive)
		{
			const auto FirstParts = SplitRelativePath(First);
			const auto SecondParts = SplitRelativePath(Second);
			if (FirstParts.size() != SecondParts.size()) return false;
			for (size_t Index = 0; Index < FirstParts.size(); ++Index)
				if (!FilesystemComponentEqual(FirstParts[Index], SecondParts[Index], CaseSensitive)) return false;
			return true;
		}

		bool FilesystemPathIsAncestor(std::string_view Parent, std::string_view Child, bool CaseSensitive)
		{
			const auto ParentParts = SplitRelativePath(Parent);
			const auto ChildParts = SplitRelativePath(Child);
			if (ParentParts.empty() || ParentParts.size() >= ChildParts.size()) return false;
			for (size_t Index = 0; Index < ParentParts.size(); ++Index)
				if (!FilesystemComponentEqual(ParentParts[Index], ChildParts[Index], CaseSensitive)) return false;
			return true;
		}

		bool ExactPathIsAncestor(std::string_view Parent, std::string_view Child)
		{
			const auto ParentParts = SplitRelativePath(Parent);
			const auto ChildParts = SplitRelativePath(Child);
			if (ParentParts.empty() || ParentParts.size() >= ChildParts.size()) return false;
			return std::equal(ParentParts.begin(), ParentParts.end(), ChildParts.begin());
		}

		std::string_view RoleName(ExportFileRole Role)
		{
			switch (Role)
			{
			case ExportFileRole::Template: return "template";
			case ExportFileRole::Cooked: return "cooked";
			case ExportFileRole::RuntimeConfiguration: return "runtime-configuration";
			case ExportFileRole::TemplateOwnership: return "template-ownership";
			case ExportFileRole::Inventory: return "inventory";
			}
			return "unknown";
		}

		std::string TemplateMembershipDigest(const std::set<std::string>& TemplateFiles, std::string_view Entrypoint)
		{
			AssetFingerprintBuilder Builder("pulseforge-export-template-membership-v1");
			Builder.UpdateText("entrypoint", Entrypoint);
			for (const std::string& Name : TemplateFiles) Builder.UpdateText("file", Name);
			return Builder.Finish().DigestHex;
		}

		void AddParentDirectoryReceipts(std::map<std::string, std::string>& Tree, std::string_view RelativeFile)
		{
			size_t Separator = RelativeFile.rfind('/');
			while (Separator != std::string_view::npos)
			{
				Tree.try_emplace("D:" + std::string(RelativeFile.substr(0, Separator)), "directory");
				if (Separator == 0) break;
				Separator = RelativeFile.rfind('/', Separator - 1);
			}
		}

		AssetContentFingerprint FingerprintText(std::string_view Text)
		{
			const auto Bytes = std::as_bytes(std::span(Text.data(), Text.size()));
			return ComputeAssetContentFingerprint(Bytes);
		}

		bool IsSafeTemplateRuntimePath(std::string_view Name, std::string_view ResourcePrefix, bool CaseSensitive)
		{
			if (!IsSafeRelative(Name)) return false;
			const std::string ResourceConfiguration = std::string(ResourcePrefix) + std::string(RuntimeConfigurationName);
			if (FilesystemPathsEqual(Name, InventoryName, CaseSensitive) ||
				FilesystemPathsEqual(Name, TemplateOwnershipName, CaseSensitive) ||
				FilesystemPathsEqual(Name, RuntimeConfigurationName, CaseSensitive) ||
				FilesystemPathsEqual(Name, ResourceConfiguration, CaseSensitive)) return false;
			const std::string CookedRoot = std::string(ResourcePrefix) + "Content";
			if (FilesystemPathsEqual(Name, CookedRoot, CaseSensitive) ||
				FilesystemPathIsAncestor(CookedRoot, Name, CaseSensitive) ||
				FilesystemPathIsAncestor(Name, CookedRoot, CaseSensitive)) return false;
			if (FilesystemPathsEqual(Name, CookStagingDirectoryName, CaseSensitive) ||
				FilesystemPathIsAncestor(CookStagingDirectoryName, Name, CaseSensitive)) return false;
			return true;
		}

		std::expected<bool, AssetExportError> EquivalentExistingPaths(
			const std::filesystem::path& First, const std::filesystem::path& Second)
		{
			std::error_code Error;
			const bool Equivalent = std::filesystem::equivalent(First, Second, Error);
			if (Error == std::errc::no_such_file_or_directory || Error == std::errc::not_a_directory) return false;
			if (Error) return std::unexpected(MakeError(AssetExportErrorCode::PublicationFailed,
				"Could not compare existing package paths: " + Error.message(), First));
			return Equivalent;
		}

		std::string JoinPathPrefix(const std::vector<std::string_view>& Parts, size_t Count)
		{
			std::string Result;
			for (size_t Index = 0; Index < Count; ++Index)
			{
				if (Index != 0) Result.push_back('/');
				Result.append(Parts[Index]);
			}
			return Result;
		}

		std::expected<void, AssetExportError> CheckExistingFilesystemAliases(
			const std::filesystem::path& Output, std::string_view Destination,
			const std::map<std::string, std::string>& ExistingTree, bool CaseSensitive)
		{
			const auto Parts = SplitRelativePath(Destination);
			for (size_t Count = 1; Count <= Parts.size(); ++Count)
			{
				const std::string Prefix = JoinPathPrefix(Parts, Count);
				if (ExistingTree.contains("F:" + Prefix) || ExistingTree.contains("D:" + Prefix)) continue;
				const auto PrefixPath = Output / std::filesystem::u8path(Prefix);
				std::error_code Error;
				const auto PrefixStatus = std::filesystem::symlink_status(PrefixPath, Error);
				if (Error == std::errc::no_such_file_or_directory) continue;
				if (Error) return std::unexpected(MakeError(AssetExportErrorCode::PublicationFailed,
					"Could not inspect a filesystem-equivalent package path: " + Error.message(), PrefixPath));
				if (!std::filesystem::exists(PrefixStatus)) continue;

				for (const auto& [EncodedPath, Digest] : ExistingTree)
				{
					(void)Digest;
					const std::string_view ExistingPath = std::string_view(EncodedPath).substr(2);
					if (SplitRelativePath(ExistingPath).size() != Count ||
						FilesystemPathsEqual(Prefix, ExistingPath, CaseSensitive)) continue;
					auto Equivalent = EquivalentExistingPaths(PrefixPath, Output / std::filesystem::u8path(ExistingPath));
					if (!Equivalent) return std::unexpected(Equivalent.error());
					if (*Equivalent)
						return std::unexpected(MakeError(AssetExportErrorCode::PublicationFailed,
							"A filesystem-equivalent package path alias conflicts with the new export destination",
							Output / std::filesystem::u8path(ExistingPath)));
				}
			}
			return {};
		}

		std::expected<void, AssetExportError> PreflightDestinations(
			const std::filesystem::path& Output,
			const std::map<std::string, std::string>& ExistingTree,
			const std::set<std::string>& PreviousOwned,
			const std::vector<ExportFileDestination>& Destinations, bool CaseSensitive)
		{
			for (size_t Index = 0; Index < Destinations.size(); ++Index)
			{
				const ExportFileDestination& Destination = Destinations[Index];
				if (!IsSafeRelative(Destination.Path))
					return std::unexpected(MakeError(AssetExportErrorCode::TemplateIncompatible,
						"Export destination contains an unsafe relative path", Output / std::filesystem::u8path(Destination.Path)));
				for (size_t OtherIndex = Index + 1; OtherIndex < Destinations.size(); ++OtherIndex)
				{
					const std::string& Other = Destinations[OtherIndex].Path;
					if (FilesystemPathsEqual(Destination.Path, Other, CaseSensitive))
						return std::unexpected(MakeError(AssetExportErrorCode::TemplateIncompatible,
							"Export roles claim the same or filesystem-equivalent destination",
							Output / std::filesystem::u8path(Destination.Path)));
					if (FilesystemPathIsAncestor(Destination.Path, Other, CaseSensitive) ||
						FilesystemPathIsAncestor(Other, Destination.Path, CaseSensitive))
						return std::unexpected(MakeError(AssetExportErrorCode::TemplateIncompatible,
							"Export file destinations have a file/directory prefix conflict",
							Output / std::filesystem::u8path(Destination.Path)));
				}
				if (auto Aliases = CheckExistingFilesystemAliases(Output, Destination.Path, ExistingTree, CaseSensitive); !Aliases)
					return std::unexpected(Aliases.error());

				for (const auto& [EncodedPath, Digest] : ExistingTree)
				{
					(void)Digest;
					const bool ExistingIsFile = EncodedPath.starts_with("F:");
					const std::string_view ExistingPath = std::string_view(EncodedPath).substr(2);
					if (FilesystemPathsEqual(Destination.Path, ExistingPath, CaseSensitive))
					{
						if (Destination.Path != ExistingPath)
							return std::unexpected(MakeError(AssetExportErrorCode::PublicationFailed,
								"A case-equivalent destination alias already exists in the package", Output / std::filesystem::u8path(ExistingPath)));
						if (!ExistingIsFile)
							return std::unexpected(MakeError(AssetExportErrorCode::PublicationFailed,
								"A package directory conflicts with a new export file", Output / std::filesystem::u8path(ExistingPath)));
						const bool InventoryIsValidated = Destination.Role == ExportFileRole::Inventory &&
							ExistingPath == InventoryName;
						if (!InventoryIsValidated && !PreviousOwned.contains(std::string(ExistingPath)))
							return std::unexpected(MakeError(AssetExportErrorCode::PublicationFailed,
								"An unowned package file collides with a new export destination; move it or choose another output",
								Output / std::filesystem::u8path(ExistingPath)));
						continue;
					}
					if (FilesystemPathIsAncestor(ExistingPath, Destination.Path, CaseSensitive))
					{
						if (ExistingIsFile)
							return std::unexpected(MakeError(AssetExportErrorCode::PublicationFailed,
								"An existing package file blocks a new export directory", Output / std::filesystem::u8path(ExistingPath)));
						if (!ExactPathIsAncestor(ExistingPath, Destination.Path))
							return std::unexpected(MakeError(AssetExportErrorCode::PublicationFailed,
								"A case-equivalent package directory alias conflicts with the new export path",
								Output / std::filesystem::u8path(ExistingPath)));
						continue;
					}
					if (FilesystemPathIsAncestor(Destination.Path, ExistingPath, CaseSensitive))
						return std::unexpected(MakeError(AssetExportErrorCode::PublicationFailed,
							"An existing package entry conflicts with a new export file path",
							Output / std::filesystem::u8path(ExistingPath)));
				}
			}
			return {};
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

		std::expected<bool, AssetExportError> ProbeFilesystemCaseSensitivity(const std::filesystem::path& OutputParent)
		{
			if (auto Safe = CheckNoLinks(OutputParent); !Safe) return std::unexpected(Safe.error());
			std::filesystem::path ProbeParent = OutputParent;
			while (!ProbeParent.empty())
			{
				std::error_code Error;
				const auto Status = std::filesystem::symlink_status(ProbeParent, Error);
				if (Error == std::errc::no_such_file_or_directory)
				{
					if (ProbeParent == ProbeParent.root_path()) break;
					ProbeParent = ProbeParent.parent_path();
					continue;
				}
				if (Error) return std::unexpected(MakeError(AssetExportErrorCode::InvalidRequest,
					"Could not inspect the nearest existing export parent: " + Error.message(), ProbeParent));
				if (std::filesystem::is_symlink(Status) || !std::filesystem::is_directory(Status))
					return std::unexpected(MakeError(AssetExportErrorCode::InvalidRequest,
						"The nearest existing export parent is linked or is not a directory", ProbeParent));
				break;
			}
			if (ProbeParent.empty())
				return std::unexpected(MakeError(AssetExportErrorCode::InvalidRequest,
					"Could not find an existing directory for the filesystem case-sensitivity probe", OutputParent));

			for (size_t Attempt = 0; Attempt < 4; ++Attempt)
			{
				auto ProbeID = UUID::Generate();
				if (!ProbeID) return std::unexpected(MakeError(AssetExportErrorCode::InternalFailure,
					"Could not generate a unique filesystem probe name: " + ProbeID.error().Message, ProbeParent));
				const std::string Name = ".pf-CaseProbe-" + ProbeID->ToString();
				std::string AliasName = Name;
				AliasName[4] = 'c';
				const auto ProbePath = ProbeParent / std::filesystem::u8path(Name);
			const auto AliasPath = ProbeParent / std::filesystem::u8path(AliasName);
#if defined(_WIN32)
				HANDLE ProbeHandle = CreateFileW(ProbePath.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE | FILE_READ_ATTRIBUTES,
					FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
				if (ProbeHandle == INVALID_HANDLE_VALUE)
				{
					const DWORD Code = GetLastError();
					if (Code == ERROR_FILE_EXISTS || Code == ERROR_ALREADY_EXISTS) continue;
					const std::error_code Error(static_cast<int>(Code), std::system_category());
					return std::unexpected(MakeError(AssetExportErrorCode::InvalidRequest,
						"Could not create an exclusive filesystem case-sensitivity probe: " + Error.message(), ProbePath));
				}

				const auto Cleanup = [&ProbeHandle, &ProbePath]() -> std::expected<void, AssetExportError>
				{
					FILE_DISPOSITION_INFO Disposition{};
					Disposition.DeleteFile = TRUE;
					if (!SetFileInformationByHandle(ProbeHandle, FileDispositionInfo, &Disposition, sizeof(Disposition)))
					{
						const std::error_code Error(static_cast<int>(GetLastError()), std::system_category());
						CloseHandle(ProbeHandle);
						ProbeHandle = INVALID_HANDLE_VALUE;
						return std::unexpected(MakeError(AssetExportErrorCode::InvalidRequest,
							"Could not safely remove the filesystem case-sensitivity probe: " + Error.message(), ProbePath));
					}
					if (!CloseHandle(ProbeHandle))
					{
						const std::error_code Error(static_cast<int>(GetLastError()), std::system_category());
						ProbeHandle = INVALID_HANDLE_VALUE;
						return std::unexpected(MakeError(AssetExportErrorCode::InvalidRequest,
							"Could not close the filesystem case-sensitivity probe: " + Error.message(), ProbePath));
					}
					ProbeHandle = INVALID_HANDLE_VALUE;
					if (GetFileAttributesW(ProbePath.c_str()) != INVALID_FILE_ATTRIBUTES)
						return std::unexpected(MakeError(AssetExportErrorCode::InvalidRequest,
							"The filesystem case-sensitivity probe still exists after cleanup", ProbePath));
					const DWORD Code = GetLastError();
					if (Code != ERROR_FILE_NOT_FOUND && Code != ERROR_PATH_NOT_FOUND)
					{
						const std::error_code Error(static_cast<int>(Code), std::system_category());
						return std::unexpected(MakeError(AssetExportErrorCode::InvalidRequest,
							"Could not verify filesystem case-sensitivity probe cleanup: " + Error.message(), ProbePath));
					}
					return {};
				};

				bool CaseSensitive = false;
				HANDLE AliasHandle = CreateFileW(AliasPath.c_str(), FILE_READ_ATTRIBUTES,
					FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
					FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
				if (AliasHandle == INVALID_HANDLE_VALUE)
				{
					const DWORD Code = GetLastError();
					if (Code == ERROR_FILE_NOT_FOUND || Code == ERROR_PATH_NOT_FOUND) CaseSensitive = true;
					else
					{
						const std::error_code Error(static_cast<int>(Code), std::system_category());
						auto Removed = Cleanup();
						std::string Message = "Could not inspect the case-alternate filesystem probe: " + Error.message();
						if (!Removed) Message += "; cleanup also failed: " + Removed.error().Message;
						return std::unexpected(MakeError(AssetExportErrorCode::InvalidRequest, std::move(Message), AliasPath));
					}
				}
				else
				{
					BY_HANDLE_FILE_INFORMATION OriginalInformation{}, AliasInformation{};
					const bool OriginalRead = GetFileInformationByHandle(ProbeHandle, &OriginalInformation) != 0;
					const bool AliasRead = GetFileInformationByHandle(AliasHandle, &AliasInformation) != 0;
					const DWORD ReadCode = GetLastError();
					const bool AliasClosed = CloseHandle(AliasHandle) != 0;
					if (!OriginalRead || !AliasRead || !AliasClosed)
					{
						const std::error_code Error(static_cast<int>(ReadCode), std::system_category());
						auto Removed = Cleanup();
						std::string Message = "Could not compare filesystem case-probe identities: " + Error.message();
						if (!Removed) Message += "; cleanup also failed: " + Removed.error().Message;
						return std::unexpected(MakeError(AssetExportErrorCode::InvalidRequest, std::move(Message), ProbePath));
					}
					CaseSensitive = OriginalInformation.dwVolumeSerialNumber != AliasInformation.dwVolumeSerialNumber ||
						OriginalInformation.nFileIndexHigh != AliasInformation.nFileIndexHigh ||
						OriginalInformation.nFileIndexLow != AliasInformation.nFileIndexLow;
				}
				auto Removed = Cleanup();
				if (!Removed) return std::unexpected(Removed.error());
				return CaseSensitive;
#else
				const int ProbeDescriptor = open(ProbePath.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
				if (ProbeDescriptor < 0)
				{
					if (errno == EEXIST) continue;
					const std::error_code Error(errno, std::generic_category());
					return std::unexpected(MakeError(AssetExportErrorCode::InvalidRequest,
						"Could not create an exclusive filesystem case-sensitivity probe: " + Error.message(), ProbePath));
				}

				struct stat OriginalStatus{};
			if (fstat(ProbeDescriptor, &OriginalStatus) != 0)
			{
				const std::error_code Error(errno, std::generic_category());
				close(ProbeDescriptor);
				const bool Removed = unlink(ProbePath.c_str()) == 0;
				std::string Message = "Could not inspect the exclusive filesystem probe: " + Error.message();
				if (!Removed) Message += "; cleanup also failed: " + std::error_code(errno, std::generic_category()).message();
				return std::unexpected(MakeError(AssetExportErrorCode::InvalidRequest, std::move(Message), ProbePath));
			}
			bool CaseSensitive = false;
			struct stat AliasStatus{};
			if (fstatat(AT_FDCWD, AliasPath.c_str(), &AliasStatus, AT_SYMLINK_NOFOLLOW) == 0)
				CaseSensitive = OriginalStatus.st_dev != AliasStatus.st_dev || OriginalStatus.st_ino != AliasStatus.st_ino;
			else if (errno == ENOENT) CaseSensitive = true;
			else
			{
				const std::error_code Error(errno, std::generic_category());
				struct stat CurrentStatus{};
				bool Removed = false;
				if (fstatat(AT_FDCWD, ProbePath.c_str(), &CurrentStatus, AT_SYMLINK_NOFOLLOW) == 0 &&
					CurrentStatus.st_dev == OriginalStatus.st_dev && CurrentStatus.st_ino == OriginalStatus.st_ino)
					Removed = unlink(ProbePath.c_str()) == 0;
				close(ProbeDescriptor);
				std::string Message = "Could not inspect the case-alternate filesystem probe: " + Error.message();
				if (!Removed) Message += "; cleanup could not safely remove the probe";
				return std::unexpected(MakeError(AssetExportErrorCode::InvalidRequest, std::move(Message), AliasPath));
			}

			struct stat CurrentStatus{};
			if (fstatat(AT_FDCWD, ProbePath.c_str(), &CurrentStatus, AT_SYMLINK_NOFOLLOW) == 0)
			{
				if (CurrentStatus.st_dev != OriginalStatus.st_dev || CurrentStatus.st_ino != OriginalStatus.st_ino)
				{
					close(ProbeDescriptor);
					return std::unexpected(MakeError(AssetExportErrorCode::InvalidRequest,
						"The filesystem probe path no longer names the exclusively created entry; leaving it untouched", ProbePath));
				}
				if (unlink(ProbePath.c_str()) != 0)
				{
					const std::error_code Error(errno, std::generic_category());
					close(ProbeDescriptor);
					return std::unexpected(MakeError(AssetExportErrorCode::InvalidRequest,
						"Could not safely remove the filesystem case-sensitivity probe: " + Error.message(), ProbePath));
				}
			}
			else if (errno != ENOENT)
			{
				const std::error_code Error(errno, std::generic_category());
				close(ProbeDescriptor);
				return std::unexpected(MakeError(AssetExportErrorCode::InvalidRequest,
					"Could not verify the filesystem case-sensitivity probe before cleanup: " + Error.message(), ProbePath));
			}
			if (close(ProbeDescriptor) != 0)
				return std::unexpected(MakeError(AssetExportErrorCode::InvalidRequest,
					"Could not close the filesystem case-sensitivity probe: " + std::error_code(errno, std::generic_category()).message(), ProbePath));
			struct stat AfterCleanup{};
			if (fstatat(AT_FDCWD, ProbePath.c_str(), &AfterCleanup, AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT)
				return std::unexpected(MakeError(AssetExportErrorCode::InvalidRequest,
					"Could not verify filesystem case-sensitivity probe cleanup", ProbePath));
			return CaseSensitive;
#endif
			}
			return std::unexpected(MakeError(AssetExportErrorCode::InvalidRequest,
				"Could not reserve a unique filesystem case-sensitivity probe name", ProbeParent));
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

		std::expected<AssetContentFingerprint, AssetExportError> Fingerprint(
			const std::filesystem::path& Path, std::stop_token CancellationToken = {})
		{
			if (CancellationToken.stop_requested())
				return std::unexpected(MakeError(AssetExportErrorCode::Cancelled, "Export was cancelled before fingerprinting a package file", Path));
			std::error_code Error;
			const uintmax_t Size = std::filesystem::file_size(Path, Error);
			if (Error || Size > MaximumExportFileBytes ||
				Size > static_cast<uintmax_t>((std::numeric_limits<std::streamsize>::max)()))
				return std::unexpected(MakeError(AssetExportErrorCode::VerificationFailed,
					Error ? "Could not inspect file for export fingerprinting: " + Error.message()
						: "File exceeds the export size limit", Path));
			std::ifstream Input(Path, std::ios::binary);
			if (!Input) return std::unexpected(MakeError(AssetExportErrorCode::VerificationFailed,
				"Could not open file for export fingerprinting", Path));
			AssetFingerprintBuilder Builder("PulseForge.AssetInputBytes.v1");
			Builder.UpdateText("byteLength", std::to_string(Size));
			std::array<std::byte, 64 * 1024> Buffer{};
			uint64_t Remaining = Size;
			while (Remaining != 0)
			{
				if (CancellationToken.stop_requested())
					return std::unexpected(MakeError(AssetExportErrorCode::Cancelled,
						"Export was cancelled while fingerprinting a package file", Path));
				const size_t Count = static_cast<size_t>(std::min<uint64_t>(Remaining, Buffer.size()));
				Input.read(reinterpret_cast<char*>(Buffer.data()), static_cast<std::streamsize>(Count));
				if (!Input || Input.bad() || static_cast<size_t>(Input.gcount()) != Count)
					return std::unexpected(MakeError(AssetExportErrorCode::VerificationFailed,
						"Could not read the complete file for export fingerprinting", Path));
				Builder.UpdateBytes("chunk", std::span<const std::byte>(Buffer.data(), Count));
				Remaining -= Count;
			}
			if (CancellationToken.stop_requested())
				return std::unexpected(MakeError(AssetExportErrorCode::Cancelled,
					"Export was cancelled after fingerprinting a package file", Path));
			Error.clear();
			const uintmax_t FinalSize = std::filesystem::file_size(Path, Error);
			if (Error || FinalSize != Size)
				return std::unexpected(MakeError(AssetExportErrorCode::VerificationFailed,
					"File size changed while export fingerprinting was in progress", Path));
			return Builder.Finish();
		}

		std::expected<void, AssetExportError> CopyOne(const std::filesystem::path& Source,
			const std::filesystem::path& Destination, std::stop_token CancellationToken = {})
		{
			if (CancellationToken.stop_requested())
				return std::unexpected(MakeError(AssetExportErrorCode::Cancelled,
					"Export was cancelled before copying a package file", Source));
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

		std::expected<std::vector<std::string>, AssetExportError> EnumerateFiles(
			const std::filesystem::path& Root, std::stop_token CancellationToken = {})
		{
			std::vector<std::string> Files;
			std::error_code Error;
			std::filesystem::recursive_directory_iterator It(Root, std::filesystem::directory_options::none, Error), End;
			if (Error) return std::unexpected(MakeError(AssetExportErrorCode::VerificationFailed,
				"Could not start package enumeration: " + Error.message(), Root));
			while (It != End)
			{
				if (CancellationToken.stop_requested())
					return std::unexpected(MakeError(AssetExportErrorCode::Cancelled,
						"Export was cancelled while enumerating package files", Root));
				const auto EntryPath = It->path();
				const auto Status = It->symlink_status(Error);
				if (Error || std::filesystem::is_symlink(Status))
					return std::unexpected(MakeError(AssetExportErrorCode::VerificationFailed,
						"Package contains an unreadable or linked entry: " + Error.message(), EntryPath));
				if (std::filesystem::is_regular_file(Status))
				{
					auto Relative = std::filesystem::relative(EntryPath, Root, Error);
					if (Error) return std::unexpected(MakeError(AssetExportErrorCode::VerificationFailed,
						"Could not form package-relative path: " + Error.message(), EntryPath));
					const auto U8 = Relative.generic_u8string();
					std::string Name(reinterpret_cast<const char*>(U8.data()), U8.size());
					if (!IsSafeRelative(Name)) return std::unexpected(MakeError(AssetExportErrorCode::VerificationFailed,
						"Package contains an unsafe relative path", EntryPath));
					Files.push_back(std::move(Name));
					if (Files.size() > MaximumExportFileCount)
						return std::unexpected(MakeError(AssetExportErrorCode::VerificationFailed, "Package file count exceeds its limit", Root));
				}
				It.increment(Error);
				if (Error) return std::unexpected(MakeError(AssetExportErrorCode::VerificationFailed,
					"Could not continue package enumeration: " + Error.message(), EntryPath));
			}
			std::sort(Files.begin(), Files.end());
			return Files;
		}

		std::expected<std::map<std::string, std::string>, AssetExportError> CaptureTree(
			const std::filesystem::path& Root, std::stop_token CancellationToken = {})
		{
			std::map<std::string, std::string> Result;
			std::error_code Error;
			std::filesystem::recursive_directory_iterator It(Root, std::filesystem::directory_options::none, Error), End;
			if (Error) return std::unexpected(MakeError(AssetExportErrorCode::VerificationFailed,
				"Could not start export tree enumeration: " + Error.message(), Root));
			while (It != End)
			{
				if (CancellationToken.stop_requested())
					return std::unexpected(MakeError(AssetExportErrorCode::Cancelled,
						"Export was cancelled while validating a package tree", Root));
				const auto EntryPath = It->path();
				const auto Status = It->symlink_status(Error);
				if (Error || std::filesystem::is_symlink(Status) ||
					(!std::filesystem::is_directory(Status) && !std::filesystem::is_regular_file(Status)))
					return std::unexpected(MakeError(AssetExportErrorCode::VerificationFailed,
						"Export tree contains an unreadable, linked, or unsupported filesystem object: " + Error.message(), EntryPath));
				auto Relative = std::filesystem::relative(EntryPath, Root, Error);
				if (Error) return std::unexpected(MakeError(AssetExportErrorCode::VerificationFailed,
					"Could not form export-relative path: " + Error.message(), EntryPath));
				const auto U8 = Relative.generic_u8string();
				const std::string Name(reinterpret_cast<const char*>(U8.data()), U8.size());
				if (!IsSafeRelative(Name)) return std::unexpected(MakeError(AssetExportErrorCode::VerificationFailed,
					"Export tree contains an unsafe path", EntryPath));
				if (std::filesystem::is_directory(Status))
					Result.emplace("D:" + Name, "directory");
				else
				{
					auto Digest = Fingerprint(EntryPath, CancellationToken);
					if (!Digest) return std::unexpected(Digest.error());
					Result.emplace("F:" + Name, Digest->DigestHex);
				}
				if (Result.size() > MaximumExportFileCount * 2)
					return std::unexpected(MakeError(AssetExportErrorCode::VerificationFailed,
						"Export tree exceeds its entry limit", Root));
				It.increment(Error);
				if (Error) return std::unexpected(MakeError(AssetExportErrorCode::VerificationFailed,
					"Could not continue export tree enumeration: " + Error.message(), EntryPath));
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
					(Record.State != RecoveryStateStaging && Record.State != RecoveryStateStagingCleanupPending &&
						Record.State != RecoveryStatePrepared && Record.State != RecoveryStateOldMoved &&
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

		bool IsTreeRemainder(
			const std::map<std::string, std::string>& Actual,
			const std::map<std::string, std::string>& Expected)
		{
			for (const auto& [Name, Digest] : Actual)
			{
				const auto It = Expected.find(Name);
				if (It == Expected.end() || It->second != Digest) return false;
			}
			return true;
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
			if ((AllowPreviouslyRemovedEntries && !IsTreeRemainder(*Actual, Expected)) ||
				(!AllowPreviouslyRemovedEntries && *Actual != Expected))
				return std::unexpected(MakeError(AssetExportErrorCode::RecoveryRequired,
					"Export recovery tree contains an unowned, missing, or independently changed entry", Root));

			// Delete only entries that were observed and matched above. Recursive remove_all could also erase
			// a new unrecorded entry created after the validation boundary. Empty-directory removal fails closed.
			for (const auto& [Name, ExpectedDigest] : *Actual)
			{
				if (!Name.starts_with("F:")) continue;
				const auto File = Root / std::filesystem::u8path(Name.substr(2));
				if (auto Safe = CheckNoLinks(File); !Safe)
					return std::unexpected(MakeError(AssetExportErrorCode::RecoveryRequired, Safe.error().Message, Safe.error().Path));
				const auto FileStatus = std::filesystem::symlink_status(File, Error);
				if (Error == std::errc::no_such_file_or_directory && AllowPreviouslyRemovedEntries) { Error.clear(); continue; }
				if (Error || !std::filesystem::is_regular_file(FileStatus))
					return std::unexpected(MakeError(AssetExportErrorCode::RecoveryRequired,
						"A validated recovery file changed type or became unobservable before deletion: " + Error.message(), File));
				auto CurrentDigest = Fingerprint(File);
				if (!CurrentDigest || CurrentDigest->DigestHex != ExpectedDigest)
					return std::unexpected(MakeError(AssetExportErrorCode::RecoveryRequired,
						"A recovery file changed after tree validation; preserving it", File));
				if (!std::filesystem::remove(File, Error) || Error)
					return std::unexpected(MakeError(AssetExportErrorCode::RecoveryRequired,
						"Could not remove a validated recovery file: " + Error.message(), File));
			}

			std::vector<std::string> Directories;
			for (const auto& [Name, Digest] : *Actual)
			{
				(void)Digest;
				if (Name.starts_with("D:")) Directories.push_back(Name.substr(2));
			}
			std::sort(Directories.begin(), Directories.end(), [](const std::string& First, const std::string& Second)
			{
				const size_t FirstDepth = static_cast<size_t>(std::count(First.begin(), First.end(), '/'));
				const size_t SecondDepth = static_cast<size_t>(std::count(Second.begin(), Second.end(), '/'));
				return FirstDepth == SecondDepth ? First.size() > Second.size() : FirstDepth > SecondDepth;
			});
			for (const std::string& Name : Directories)
			{
				const auto Directory = Root / std::filesystem::u8path(Name);
				if (auto Safe = CheckNoLinks(Directory); !Safe)
					return std::unexpected(MakeError(AssetExportErrorCode::RecoveryRequired, Safe.error().Message, Safe.error().Path));
				const auto DirectoryStatus = std::filesystem::symlink_status(Directory, Error);
				if (Error == std::errc::no_such_file_or_directory && AllowPreviouslyRemovedEntries) { Error.clear(); continue; }
				if (Error || !std::filesystem::is_directory(DirectoryStatus))
					return std::unexpected(MakeError(AssetExportErrorCode::RecoveryRequired,
						"A validated recovery directory changed type or became unobservable: " + Error.message(), Directory));
				if (!std::filesystem::remove(Directory, Error) || Error)
					return std::unexpected(MakeError(AssetExportErrorCode::RecoveryRequired,
						"A recovery directory contains an unrecorded entry or could not be removed: " + Error.message(), Directory));
			}

			if (!std::filesystem::remove(Root, Error) || Error)
				return std::unexpected(MakeError(AssetExportErrorCode::RecoveryRequired,
					"Validated recovery root is no longer empty or could not be removed: " + Error.message(), Root));
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
			const bool CandidateIsStagingRemainder = CandidateExists &&
				(Record.State == RecoveryStateStaging || Record.State == RecoveryStateStagingCleanupPending) &&
				IsTreeRemainder(CandidateTree, Record.CandidateTree);
			if ((OutputExists && !OutputIsNew && !OutputIsOld) ||
				(BackupExists && !BackupIsOld) || (CandidateExists && !CandidateIsNew && !CandidateIsStagingRemainder))
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
				if ((Record.State == RecoveryStateStaging || Record.State == RecoveryStateStagingCleanupPending) && !CandidateExists)
					return RemoveJournal(JournalPath);
				if (CandidateIsStagingRemainder)
				{
					if (auto Removed = RemoveValidatedTree(Record.Candidate, Record.CandidateTree, true); !Removed)
						return std::unexpected(Removed.error());
					return RemoveJournal(JournalPath);
				}
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
				if (CandidateIsStagingRemainder)
				{
					if (auto Removed = RemoveValidatedTree(Record.Candidate, Record.CandidateTree, true); !Removed)
						return std::unexpected(Removed.error());
				}
				else if (CandidateExists && CandidateIsNew)
				{
					if (auto Removed = RemoveValidatedTree(Record.Candidate, Record.CandidateTree, false); !Removed)
						return std::unexpected(Removed.error());
				}
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

		std::string_view ResourcePrefixForPlatform(ExportTargetPlatform Value)
		{
			return Value == ExportTargetPlatform::MacOS ? "Contents/Resources/" : "";
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
			if (PermissionError || (Permissions & std::filesystem::perms::owner_exec) == std::filesystem::perms::none)
				return std::unexpected(MakeError(AssetExportErrorCode::TemplateIncomplete,
					"Template entrypoint is not executable by the export owner on this platform", Path));
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

		std::expected<ExportInventoryOwnership, AssetExportError> ValidateInventory(
			const std::filesystem::path& Existing, const std::set<std::string>& CurrentTemplateFiles,
			std::string_view ResourcePrefix, std::string_view ExpectedProfileID, std::string_view ExpectedProjectID,
			bool CaseSensitive)
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
				const uint32_t Version = Inventory.is_object() ? Inventory.value("version", 0u) : 0u;
				const size_t ExpectedFieldCount = Version == 2 ? 6 : Version == 3 ? 7 : 0;
				if (DuplicateKey || !Inventory.is_object() || ExpectedFieldCount == 0 || Inventory.size() != ExpectedFieldCount ||
					Inventory.value("format", std::string{}) != "PulseForgeExportInventory" ||
					Inventory.value("profileId", std::string{}) != ExpectedProfileID ||
					Inventory.value("projectId", std::string{}) != ExpectedProjectID ||
					Inventory.value("cookGeneration", std::string{}).empty() ||
					!Inventory.contains("files") || !Inventory["files"].is_object())
					return std::unexpected(MakeError(AssetExportErrorCode::PublicationFailed, "Existing export inventory is invalid", InventoryPath));
				if (Version == 3 && Inventory.value("resourcePrefix", std::string{}) != std::string(ResourcePrefix))
					return std::unexpected(MakeError(AssetExportErrorCode::PublicationFailed,
						"Existing export inventory targets a different runtime resource layout", InventoryPath));

				ExportInventoryOwnership Ownership;
				if (Version == 3)
				{
					const auto OwnershipReceipt = Inventory["files"].find(std::string(TemplateOwnershipName));
					if (OwnershipReceipt == Inventory["files"].end() || !OwnershipReceipt->is_object() ||
						OwnershipReceipt->value("role", std::string{}) != "template-ownership" ||
						!OwnershipReceipt->contains("digest") || !(*OwnershipReceipt)["digest"].is_string())
						return std::unexpected(MakeError(AssetExportErrorCode::PublicationFailed,
							"Version 3 export inventory is missing its separately bound template-ownership record", InventoryPath));
					const std::string OwnershipDigest = (*OwnershipReceipt)["digest"].get<std::string>();
					if (OwnershipDigest.size() != 32 || !std::all_of(OwnershipDigest.begin(), OwnershipDigest.end(),
						[](unsigned char C) { return (C >= '0' && C <= '9') || (C >= 'a' && C <= 'f'); }))
						return std::unexpected(MakeError(AssetExportErrorCode::PublicationFailed,
							"Template-ownership record digest is invalid", Existing / std::filesystem::u8path(TemplateOwnershipName)));
					const auto OwnershipPath = Existing / std::filesystem::u8path(TemplateOwnershipName);
					if (auto Safe = CheckNoLinks(OwnershipPath); !Safe)
						return std::unexpected(MakeError(AssetExportErrorCode::PublicationFailed, Safe.error().Message, Safe.error().Path));
					auto ActualOwnershipDigest = Fingerprint(OwnershipPath);
					if (!ActualOwnershipDigest || ActualOwnershipDigest->DigestHex != OwnershipDigest)
						return std::unexpected(MakeError(AssetExportErrorCode::PublicationFailed,
							"Template-ownership record changed independently; preserving existing output", OwnershipPath));
					auto OwnershipBytes = ReadFile(OwnershipPath, MaximumDescriptorBytes);
					if (!OwnershipBytes) return std::unexpected(OwnershipBytes.error());
					const std::string OwnershipText(OwnershipBytes->empty() ? "" :
						reinterpret_cast<const char*>(OwnershipBytes->data()), OwnershipBytes->size());
					std::vector<std::set<std::string>> OwnershipObjectKeys;
					bool OwnershipDuplicateKey = false;
					const auto OwnershipCallback = [&OwnershipObjectKeys, &OwnershipDuplicateKey](int, Json::parse_event_t Event, Json& Parsed)
					{
						if (Event == Json::parse_event_t::object_start) OwnershipObjectKeys.emplace_back();
						else if (Event == Json::parse_event_t::key && !OwnershipObjectKeys.empty())
							OwnershipDuplicateKey |= !OwnershipObjectKeys.back().insert(Parsed.get<std::string>()).second;
						else if (Event == Json::parse_event_t::object_end && !OwnershipObjectKeys.empty()) OwnershipObjectKeys.pop_back();
						return true;
					};
					const Json OwnershipDocument = Json::parse(OwnershipText.begin(), OwnershipText.end(), OwnershipCallback);
					if (OwnershipDuplicateKey || !OwnershipDocument.is_object() || OwnershipDocument.size() != 7 ||
						OwnershipDocument.value("format", std::string{}) != "PulseForgeTemplateOwnership" ||
						OwnershipDocument.value("version", 0u) != 1u ||
						OwnershipDocument.value("profileId", std::string{}) != ExpectedProfileID ||
						OwnershipDocument.value("projectId", std::string{}) != ExpectedProjectID ||
						!OwnershipDocument.contains("entrypoint") || !OwnershipDocument["entrypoint"].is_string() ||
						!OwnershipDocument.contains("templateFiles") || !OwnershipDocument["templateFiles"].is_array() ||
						OwnershipDocument["templateFiles"].empty() ||
						OwnershipDocument["templateFiles"].size() > MaximumExportFileCount)
						return std::unexpected(MakeError(AssetExportErrorCode::PublicationFailed,
							"Existing template-ownership record is malformed or incompatible", OwnershipPath));
					Ownership.TemplateEntrypoint = OwnershipDocument["entrypoint"].get<std::string>();
					for (const Json& Value : OwnershipDocument["templateFiles"])
					{
						if (!Value.is_string())
							return std::unexpected(MakeError(AssetExportErrorCode::PublicationFailed,
								"Template-ownership record contains a non-string path", OwnershipPath));
						const std::string Name = Value.get<std::string>();
						if (!IsSafeTemplateRuntimePath(Name, ResourcePrefix, CaseSensitive))
							return std::unexpected(MakeError(AssetExportErrorCode::PublicationFailed,
								"Template-ownership record contains an unsafe or reserved path", OwnershipPath));
						for (const std::string& ExistingName : Ownership.TemplateFiles)
						{
							bool Alias = FilesystemPathsEqual(Name, ExistingName, CaseSensitive);
							if (!Alias)
							{
								auto Equivalent = EquivalentExistingPaths(
									Existing / std::filesystem::u8path(Name), Existing / std::filesystem::u8path(ExistingName));
								if (!Equivalent) return std::unexpected(Equivalent.error());
								Alias = *Equivalent;
							}
							if (Alias)
								return std::unexpected(MakeError(AssetExportErrorCode::PublicationFailed,
									"Template-ownership record has duplicate or filesystem-equivalent paths", OwnershipPath));
						}
						Ownership.TemplateFiles.insert(Name);
					}
					const std::string MembershipDigest = OwnershipDocument.value("membershipDigest", std::string{});
					if (!IsSafeTemplateRuntimePath(Ownership.TemplateEntrypoint, ResourcePrefix, CaseSensitive) ||
						!Ownership.TemplateFiles.contains(Ownership.TemplateEntrypoint) || MembershipDigest.size() != 32 ||
						!std::all_of(MembershipDigest.begin(), MembershipDigest.end(),
							[](unsigned char C) { return (C >= '0' && C <= '9') || (C >= 'a' && C <= 'f'); }) ||
						MembershipDigest != TemplateMembershipDigest(Ownership.TemplateFiles, Ownership.TemplateEntrypoint))
						return std::unexpected(MakeError(AssetExportErrorCode::PublicationFailed,
							"Template-ownership membership or entrypoint receipt is inconsistent", OwnershipPath));
				}

				std::set<std::string> ReceiptTemplateFiles;
				for (const auto& [Name, Expected] : Inventory["files"].items())
				{
					if (!IsSafeRelative(Name) || Ownership.Files.size() >= MaximumExportFileCount)
						return std::unexpected(MakeError(AssetExportErrorCode::PublicationFailed,
							"Existing export inventory entry is invalid or exceeds its file limit", InventoryPath));
					std::string Role;
					std::string ExpectedDigest;
					if (Version == 3)
					{
						if (!Expected.is_object() || Expected.size() != 2 || !Expected.contains("role") ||
							!Expected["role"].is_string() || !Expected.contains("digest") || !Expected["digest"].is_string())
							return std::unexpected(MakeError(AssetExportErrorCode::PublicationFailed,
								"Existing export inventory file receipt is invalid", InventoryPath));
						Role = Expected["role"].get<std::string>();
						ExpectedDigest = Expected["digest"].get<std::string>();
					}
					else
					{
						if (!Expected.is_string())
							return std::unexpected(MakeError(AssetExportErrorCode::PublicationFailed,
								"Version 2 export inventory receipt is invalid", InventoryPath));
						ExpectedDigest = Expected.get<std::string>();
						if (CurrentTemplateFiles.contains(Name)) Role = "template";
						else if (Name == std::string(ResourcePrefix) + std::string(RuntimeConfigurationName)) Role = "runtime-configuration";
						else if (IsOwnedCookedProductPath(Name, ResourcePrefix)) Role = "cooked";
						else
							return std::unexpected(MakeError(AssetExportErrorCode::PublicationFailed,
								"Version 2 inventory cannot establish the role of " + Name +
								". Select the original template descriptor or use a new output path; no package files were changed.", InventoryPath));
					}
					const bool ValidDigest = ExpectedDigest.size() == 32 && std::all_of(ExpectedDigest.begin(), ExpectedDigest.end(),
						[](unsigned char C) { return (C >= '0' && C <= '9') || (C >= 'a' && C <= 'f'); });
					if (!ValidDigest)
						return std::unexpected(MakeError(AssetExportErrorCode::PublicationFailed,
							"Existing export inventory digest is invalid", InventoryPath));

					bool RolePathValid = false;
					if (Role == "template")
						RolePathValid = Version == 2 ? CurrentTemplateFiles.contains(Name) && IsSafeTemplateRuntimePath(Name, ResourcePrefix, CaseSensitive)
							: Ownership.TemplateFiles.contains(Name) && IsSafeTemplateRuntimePath(Name, ResourcePrefix, CaseSensitive);
					else if (Role == "cooked") RolePathValid = IsOwnedCookedProductPath(Name, ResourcePrefix);
					else if (Role == "runtime-configuration")
						RolePathValid = FilesystemPathsEqual(Name,
							std::string(ResourcePrefix) + std::string(RuntimeConfigurationName), CaseSensitive);
					else if (Role == "template-ownership")
						RolePathValid = Version == 3 && Name == TemplateOwnershipName;
					if (!RolePathValid)
						return std::unexpected(MakeError(AssetExportErrorCode::PublicationFailed,
							"Existing export inventory role does not permit this package path", InventoryPath));

					for (const std::string& ExistingName : Ownership.Files)
					{
						bool Alias = FilesystemPathsEqual(Name, ExistingName, CaseSensitive);
						if (!Alias)
						{
							auto Equivalent = EquivalentExistingPaths(
								Existing / std::filesystem::u8path(Name), Existing / std::filesystem::u8path(ExistingName));
							if (!Equivalent) return std::unexpected(Equivalent.error());
							Alias = *Equivalent;
						}
						if (Alias)
							return std::unexpected(MakeError(AssetExportErrorCode::PublicationFailed,
								"Existing export inventory has duplicate or case-equivalent file paths", InventoryPath));
					}
					Ownership.Files.insert(Name);
					if (Role == "template")
					{
						ReceiptTemplateFiles.insert(Name);
						if (Version == 2) Ownership.TemplateFiles.insert(Name);
					}
					if (auto Safe = CheckNoLinks(Existing / std::filesystem::u8path(Name)); !Safe)
						return std::unexpected(MakeError(AssetExportErrorCode::PublicationFailed, Safe.error().Message, Safe.error().Path));
					auto Actual = Fingerprint(Existing / std::filesystem::u8path(Name));
					if (!Actual || Actual->DigestHex != ExpectedDigest)
						return std::unexpected(MakeError(AssetExportErrorCode::PublicationFailed,
							"An owned package file changed independently; preserving existing output", Existing / std::filesystem::u8path(Name)));
				}
				if (Version == 2 && Ownership.TemplateFiles != CurrentTemplateFiles)
					return std::unexpected(MakeError(AssetExportErrorCode::PublicationFailed,
						"Version 2 inventory does not establish the prior template file list. Select the exact descriptor used for this package, or use a new output path; no package files were changed.", InventoryPath));
				if (Version == 3 && ReceiptTemplateFiles != Ownership.TemplateFiles)
					return std::unexpected(MakeError(AssetExportErrorCode::PublicationFailed,
						"Main inventory template receipts do not match the separately bound ownership record", InventoryPath));
				return Ownership;
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
		{
			if (!IsSafeTemplateRuntimePath(File, ResourcePrefixForPlatform(Descriptor.TargetPlatform), true))
				return std::unexpected(MakeError(AssetExportErrorCode::TemplateIncompatible,
					"Template runtime files contain an unsafe or reserved output path"));
			for (const std::string& Existing : Unique)
				if (FilesystemPathsEqual(File, Existing, true))
					return std::unexpected(MakeError(AssetExportErrorCode::TemplateIncompatible,
						"Template runtime files must have unique filesystem-equivalent paths"));
			Unique.insert(File);
		}
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

	std::expected<std::filesystem::path, AssetExportError> ResolveExportTemplateRoot(
		const std::filesystem::path& DescriptorPath, std::optional<std::filesystem::path> ExplicitRoot)
	{
		auto DescriptorAbsolute = Absolute(DescriptorPath);
		if (!DescriptorAbsolute) return std::unexpected(DescriptorAbsolute.error());
		if (auto Safe = CheckNoLinks(*DescriptorAbsolute); !Safe) return std::unexpected(Safe.error());
		auto Descriptor = LoadExportTemplateDescriptor(*DescriptorAbsolute);
		if (!Descriptor) return std::unexpected(Descriptor.error());

		const auto CheckRoot = [&Descriptor](const std::filesystem::path& RootPath)
			-> std::expected<std::filesystem::path, AssetExportError>
		{
			auto Root = Absolute(RootPath);
			if (!Root) return std::unexpected(Root.error());
			if (auto Safe = CheckNoLinks(*Root); !Safe) return std::unexpected(Safe.error());
			std::error_code Error;
			const auto RootStatus = std::filesystem::symlink_status(*Root, Error);
			if (Error || !std::filesystem::is_directory(RootStatus))
				return std::unexpected(MakeError(AssetExportErrorCode::TemplateMissing,
					"Template root is missing or is not a directory", *Root));
			const auto Entrypoint = *Root / std::filesystem::u8path(Descriptor->Entrypoint);
			if (auto Safe = CheckNoLinks(Entrypoint); !Safe) return std::unexpected(Safe.error());
			const auto EntrypointStatus = std::filesystem::symlink_status(Entrypoint, Error);
			if (Error || !std::filesystem::is_regular_file(EntrypointStatus))
				return std::unexpected(MakeError(AssetExportErrorCode::TemplateIncomplete,
					"Template root does not contain the descriptor entrypoint as a regular file", Entrypoint));
			return *Root;
		};

		if (ExplicitRoot) return CheckRoot(*ExplicitRoot);

		std::filesystem::path Candidate = DescriptorAbsolute->parent_path();
		while (!Candidate.empty())
		{
			const auto Entrypoint = Candidate / std::filesystem::u8path(Descriptor->Entrypoint);
			std::error_code Error;
			const auto Status = std::filesystem::symlink_status(Entrypoint, Error);
			if (!Error && std::filesystem::exists(Status)) return CheckRoot(Candidate);
			if (Error != std::errc::no_such_file_or_directory && Error)
				return std::unexpected(MakeError(AssetExportErrorCode::TemplateMissing,
					"Could not inspect a candidate template entrypoint: " + Error.message(), Entrypoint));
			if (Candidate == Candidate.root_path()) break;
			Candidate = Candidate.parent_path();
		}
		return std::unexpected(MakeError(AssetExportErrorCode::TemplateMissing,
			"Could not discover a template root containing descriptor entrypoint " + Descriptor->Entrypoint,
			*DescriptorAbsolute));
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
		m_RequestStopSource = std::stop_source{};
		const std::stop_token CancellationToken = m_RequestStopSource.get_token();
		m_Snapshot = {};
		m_PublicationStarted = false;
		m_CandidateCleanupPending = false;
		m_PendingFinish.reset();
		m_Snapshot.RequestID = ID;
		m_Snapshot.Running = true;
		m_Snapshot.Phase = AssetExportPhase::Validate;
		m_Worker = std::jthread([this, ID, Request = std::move(Request), CancellationToken]() mutable
			{ Run(ID, std::move(Request), CancellationToken); });
		return ID;
	}

	AssetExportSnapshot AssetExportService::GetSnapshot(uint64_t RequestID) const
	{
		std::lock_guard Lock(m_Mutex);
		return m_Snapshot.RequestID == RequestID ? m_Snapshot : AssetExportSnapshot{};
	}

	bool AssetExportService::RequestCancel(uint64_t RequestID) noexcept
	{
		std::stop_source StopSource;
		{
			std::lock_guard Lock(m_Mutex);
			if (m_Snapshot.RequestID != RequestID || !m_Snapshot.Running || m_PublicationStarted) return false;
			m_Snapshot.CancellationRequested = true;
			StopSource = m_RequestStopSource;
		}
		(void)StopSource.request_stop();
		return true;
	}

	void AssetExportService::Shutdown() noexcept
	{
		std::stop_source StopSource;
		bool SignalCancellation = false;
		{
			std::lock_guard Lock(m_Mutex);
			if (m_ShuttingDown && !m_Worker.joinable()) return;
			m_ShuttingDown = true;
			if (m_Snapshot.Running && !m_PublicationStarted)
			{
				m_Snapshot.CancellationRequested = true;
				StopSource = m_RequestStopSource;
				SignalCancellation = true;
			}
		}
		if (SignalCancellation) (void)StopSource.request_stop();
		if (m_Worker.joinable()) m_Worker.join();
	}

	bool AssetExportService::TryBeginPublication(uint64_t RequestID) noexcept
	{
		std::lock_guard Lock(m_Mutex);
		if (m_Snapshot.RequestID != RequestID || !m_Snapshot.Running ||
			m_Snapshot.CancellationRequested || m_RequestStopSource.stop_requested()) return false;
		m_PublicationStarted = true;
		return true;
	}

	void AssetExportService::BeginCandidateCleanup(uint64_t RequestID) noexcept
	{
		std::lock_guard Lock(m_Mutex);
		if (m_Snapshot.RequestID != RequestID || !m_Snapshot.Running) return;
		m_CandidateCleanupPending = true;
		m_PendingFinish.reset();
	}

	void AssetExportService::CompleteCandidateCleanup(uint64_t RequestID) noexcept
	{
		std::lock_guard Lock(m_Mutex);
		if (m_Snapshot.RequestID != RequestID || !m_CandidateCleanupPending) return;
		m_CandidateCleanupPending = false;
		if (!m_PendingFinish) return;
		m_Snapshot.Phase = m_PendingFinish->Phase;
		m_Snapshot.Progress = m_PendingFinish->Phase == AssetExportPhase::Complete ? 1.0f : m_Snapshot.Progress;
		m_Snapshot.Running = false;
		m_Snapshot.Error = std::move(m_PendingFinish->Error);
		m_Snapshot.PublishedOutput = std::move(m_PendingFinish->Output);
		m_PendingFinish.reset();
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
		if (m_CandidateCleanupPending)
		{
			m_PendingFinish = PendingFinish{ Phase, std::move(Error), std::move(Output) };
			return;
		}
		m_Snapshot.Phase = Phase;
		m_Snapshot.Progress = Phase == AssetExportPhase::Complete ? 1.0f : m_Snapshot.Progress;
		m_Snapshot.Running = false;
		m_Snapshot.Error = std::move(Error);
		m_Snapshot.PublishedOutput = std::move(Output);
	}

	void AssetExportService::Run(uint64_t RequestID, AssetExportRequest Request, std::stop_token CancellationToken) noexcept
	{
		const auto Cancelled = [CancellationToken]() { return CancellationToken.stop_requested(); };
		const auto FinishPackageWorkError = [this, RequestID, &Cancelled](const AssetExportError& Error)
		{
			if (Cancelled() || Error.Code == AssetExportErrorCode::Cancelled)
				Finish(RequestID, AssetExportPhase::Cancelled,
					MakeError(AssetExportErrorCode::Cancelled, "Export was cancelled during bounded package file work", Error.Path));
			else
				Finish(RequestID, AssetExportPhase::Failed, Error);
		};
		try
		{
			auto ValidProfile = ValidateProfile(Request.Profile);
			if (!ValidProfile) { Finish(RequestID, AssetExportPhase::Failed, ValidProfile.error()); return; }
			if (Request.SourceProjectFile.empty() || Request.TemplateDescriptor.empty())
				{ Finish(RequestID, AssetExportPhase::Failed, MakeError(AssetExportErrorCode::InvalidRequest, "Project and template descriptor are required")); return; }
			if (Cancelled()) { Finish(RequestID, AssetExportPhase::Cancelled, MakeError(AssetExportErrorCode::Cancelled, "Export cancelled before template resolution")); return; }
			Update(RequestID, AssetExportPhase::ResolveTemplate, 0.08f, "Validating native player template");
			std::optional<std::filesystem::path> ExplicitTemplateRoot;
			if (!Request.TemplateRoot.empty()) ExplicitTemplateRoot = Request.TemplateRoot;
			auto ResolvedTemplateRoot = ResolveExportTemplateRoot(Request.TemplateDescriptor, std::move(ExplicitTemplateRoot));
			if (!ResolvedTemplateRoot) { Finish(RequestID, AssetExportPhase::Failed, ResolvedTemplateRoot.error()); return; }
			Request.TemplateRoot = std::move(*ResolvedTemplateRoot);
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
			auto Project = Project::Open(Request.SourceProjectFile, nullptr, CancellationToken);
			if (!Project)
			{
				if (Cancelled() || Project.error().Code == ProjectErrorCode::Cancelled)
					Finish(RequestID, AssetExportPhase::Cancelled, MakeError(AssetExportErrorCode::Cancelled,
						"Export cancelled while opening the source project", Request.SourceProjectFile));
				else
					Finish(RequestID, AssetExportPhase::Failed, MakeError(AssetExportErrorCode::ProjectOpenFailed,
						Project.error().Message, Request.SourceProjectFile));
				return;
			}
			if (Cancelled()) { Finish(RequestID, AssetExportPhase::Cancelled,
				MakeError(AssetExportErrorCode::Cancelled, "Export cancelled after opening the source project")); return; }
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
			auto OutputLinks = CheckNoLinks(Output);
			if (!OutputLinks) { Finish(RequestID, AssetExportPhase::Failed, OutputLinks.error()); return; }
			std::error_code OutputStatusError;
			const auto OutputStatus = std::filesystem::symlink_status(Output, OutputStatusError);
			const bool OutputAlreadyExists = !OutputStatusError && std::filesystem::exists(OutputStatus);
			if (OutputStatusError != std::errc::no_such_file_or_directory && OutputStatusError)
				{ Finish(RequestID, AssetExportPhase::Failed, MakeError(AssetExportErrorCode::InvalidRequest,
					"Could not inspect export destination: " + OutputStatusError.message(), Output)); return; }
			if (OutputAlreadyExists && !std::filesystem::is_directory(OutputStatus))
				{ Finish(RequestID, AssetExportPhase::Failed, MakeError(AssetExportErrorCode::InvalidRequest,
					"Export destination exists and is not a directory", Output)); return; }
			auto OutputCaseSensitivity = ProbeFilesystemCaseSensitivity(OutputAlreadyExists ? Output : Output.parent_path());
			if (!OutputCaseSensitivity) { Finish(RequestID, AssetExportPhase::Failed, OutputCaseSensitivity.error()); return; }
			const bool OutputCaseSensitive = *OutputCaseSensitivity;
			if (Contains(*SourceRoot, Output, OutputCaseSensitive) || Contains(Output, *SourceRoot, OutputCaseSensitive) ||
				Contains(*Template, Output, OutputCaseSensitive) || Contains(Output, *Template, OutputCaseSensitive))
				{ Finish(RequestID, AssetExportPhase::Failed, MakeError(AssetExportErrorCode::InvalidRequest, "Export destination may not overlap project or template inputs", Output)); return; }
			Update(RequestID, AssetExportPhase::Validate, 0.20f, "Destination and source/template separation verified");
			std::error_code Error;
			std::set<std::string> PreviousOwned;
			std::map<std::string, std::string> ExistingTree;
			const std::set<std::string> TemplateFiles(Descriptor->RuntimeFiles.begin(), Descriptor->RuntimeFiles.end());
			const std::string ResourcePrefix(ResourcePrefixForPlatform(Descriptor->TargetPlatform));
			const std::filesystem::path ResourceRelative = Descriptor->TargetPlatform == ExportTargetPlatform::MacOS
				? std::filesystem::path("Contents") / "Resources" : std::filesystem::path{};
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
				auto Existing = ValidateInventory(Output, TemplateFiles, ResourcePrefix,
					Request.Profile.ProfileID.ToString(), Project->GetDescription().ID.ToString(), OutputCaseSensitive);
				if (!Existing) { Finish(RequestID, AssetExportPhase::Failed, Existing.error()); return; }
				PreviousOwned = std::move(Existing->Files);
				auto Tree = CaptureTree(Output, CancellationToken);
				if (!Tree) { FinishPackageWorkError(Tree.error()); return; }
				ExistingTree = std::move(*Tree);
			}
			std::vector<ExportFileDestination> InitialDestinations;
			InitialDestinations.reserve(Descriptor->RuntimeFiles.size() + 2);
			for (const std::string& Relative : Descriptor->RuntimeFiles)
				InitialDestinations.push_back({ Relative, ExportFileRole::Template });
			InitialDestinations.push_back({
				(ResourceRelative / RuntimeConfigurationName).generic_string(), ExportFileRole::RuntimeConfiguration });
			InitialDestinations.push_back({ std::string(TemplateOwnershipName), ExportFileRole::TemplateOwnership });
			InitialDestinations.push_back({ std::string(InventoryName), ExportFileRole::Inventory });
			if (auto Preflight = PreflightDestinations(Output, ExistingTree, PreviousOwned, InitialDestinations, OutputCaseSensitive); !Preflight)
				{ Finish(RequestID, AssetExportPhase::Failed, Preflight.error()); return; }
			for (const auto& [Entry, Digest] : ExistingTree)
			{
				(void)Digest;
				const std::string_view ExistingPath(Entry.data() + 2, Entry.size() - 2);
				if (FilesystemPathsEqual(ExistingPath, CookStagingDirectoryName, OutputCaseSensitive) ||
					FilesystemPathIsAncestor(ExistingPath, CookStagingDirectoryName, OutputCaseSensitive) ||
					FilesystemPathIsAncestor(CookStagingDirectoryName, ExistingPath, OutputCaseSensitive))
					{ Finish(RequestID, AssetExportPhase::Failed, MakeError(AssetExportErrorCode::PublicationFailed,
						"An existing package entry conflicts with the reserved cook staging path",
						Output / std::filesystem::u8path(ExistingPath))); return; }
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
			Recovery.HadPreviousOutput = OutputAlreadyExists;
			Recovery.State = RecoveryStateStaging;
			Recovery.PreviousTree = ExistingTree;
			Recovery.CandidateTree = ExistingTree;
			if (Request.BeforeCandidateCreate) Request.BeforeCandidateCreate(Candidate);
			for (const auto& Path : { Candidate, Backup })
			{
				const auto Status = std::filesystem::symlink_status(Path, Error);
				if (Error == std::errc::no_such_file_or_directory) { Error.clear(); continue; }
				if (Error || std::filesystem::exists(Status) || std::filesystem::is_symlink(Status))
				{
					Finish(RequestID, AssetExportPhase::Failed, MakeError(AssetExportErrorCode::RecoveryRequired,
						Error ? "Could not inspect a unique export staging path: " + Error.message() :
						"A unique export candidate or backup path already exists; preserving it", Path));
					return;
				}
			}
			if (auto Safe = CheckNoLinks(JournalPath); !Safe)
				{ Finish(RequestID, AssetExportPhase::Failed, Safe.error()); return; }
			if (auto Saved = SaveRecoveryRecord(JournalPath, Recovery); !Saved)
				{ Finish(RequestID, AssetExportPhase::Failed, Saved.error()); return; }
			struct RecoveryCleanup final
			{
				std::filesystem::path Path;
				bool* KeepJournal = nullptr;
				bool Keep = false;
				~RecoveryCleanup()
				{
					if (!Keep && (!KeepJournal || !*KeepJournal))
					{
						std::error_code Error;
						std::filesystem::remove(Path, Error);
					}
				}
			} JournalCleanup{ JournalPath };
			struct Cleanup final
			{
				std::filesystem::path Path;
				std::filesystem::path JournalPath;
				ExportRecoveryRecord* Recovery = nullptr;
				bool* KeepJournal = nullptr;
				std::function<void(const std::filesystem::path&)> BeforeCleanup;
				std::function<void(AssetExportError)> ReportFailure;
				std::function<void()> CompleteCleanup;
				bool Created = false;
				~Cleanup()
				{
					if (!Created || !Recovery || !KeepJournal)
					{
						if (CompleteCleanup) CompleteCleanup();
						return;
					}
					if (Path.empty())
					{
						if (CompleteCleanup) CompleteCleanup();
						return;
					}
					try
					{
						if (BeforeCleanup) BeforeCleanup(Path);
						Recovery->State = RecoveryStateStagingCleanupPending;
						auto Saved = SaveRecoveryRecord(JournalPath, *Recovery);
						if (!Saved)
						{
							*KeepJournal = true;
							if (ReportFailure) ReportFailure(MakeError(AssetExportErrorCode::RecoveryRequired,
								"Could not record ownership before candidate cleanup: " + Saved.error().Message, Path));
							if (CompleteCleanup) CompleteCleanup();
						return;
						}
						auto Removed = RemoveValidatedTree(Path, Recovery->CandidateTree, true);
						if (!Removed)
						{
							*KeepJournal = true;
							if (ReportFailure) ReportFailure(MakeError(AssetExportErrorCode::RecoveryRequired,
								"Candidate cleanup could not prove ownership; preserving the candidate and recovery record: " +
								Removed.error().Message, Path));
							if (CompleteCleanup) CompleteCleanup();
							return;
						}
						Created = false;
					}
					catch (const std::exception& Exception)
					{
						*KeepJournal = true;
						if (ReportFailure) ReportFailure(MakeError(AssetExportErrorCode::RecoveryRequired,
							std::string("Candidate cleanup failed safely: ") + Exception.what(), Path));
					}
					catch (...)
					{
						*KeepJournal = true;
						if (ReportFailure) ReportFailure(MakeError(AssetExportErrorCode::RecoveryRequired,
							"Candidate cleanup failed with an unknown exception", Path));
					}
					if (CompleteCleanup) CompleteCleanup();
				}
			} CandidateCleanup{ Candidate, JournalPath, &Recovery, &JournalCleanup.Keep,
				Request.BeforeCandidateCleanup,
				[this, RequestID](AssetExportError Error) { Finish(RequestID, AssetExportPhase::Failed, std::move(Error)); },
				[this, RequestID] { CompleteCandidateCleanup(RequestID); } };
			const bool CandidateCreated = std::filesystem::create_directory(Candidate, Error);
			if (Error || !CandidateCreated)
			{
				JournalCleanup.Keep = true;
				Finish(RequestID, AssetExportPhase::Failed, MakeError(AssetExportErrorCode::StageFailed,
					Error ? "Could not create unique export candidate: " + Error.message() :
					"Unique export candidate path was occupied before creation", Candidate));
				return;
			}
			CandidateCleanup.Created = true;
			BeginCandidateCleanup(RequestID);
			std::map<std::string, std::string> CandidateExpected = ExistingTree;
			if (OutputAlreadyExists)
			{
				for (const auto& [Entry, Digest] : ExistingTree)
				{
					if (Cancelled()) { Finish(RequestID, AssetExportPhase::Cancelled,
						MakeError(AssetExportErrorCode::Cancelled, "Export cancelled while preserving existing package directories")); return; }
					(void)Digest;
					if (!Entry.starts_with("D:")) continue;
					const auto Destination = Candidate / std::filesystem::u8path(Entry.substr(2));
					std::filesystem::create_directories(Destination, Error);
					if (Error) { Finish(RequestID, AssetExportPhase::Failed, MakeError(AssetExportErrorCode::StageFailed,
						"Could not preserve an existing package directory: " + Error.message(), Destination)); return; }
				}
				for (const auto& [Entry, ExpectedDigest] : ExistingTree)
				{
					if (Cancelled()) { Finish(RequestID, AssetExportPhase::Cancelled,
						MakeError(AssetExportErrorCode::Cancelled, "Export cancelled while preserving existing package files")); return; }
					if (Entry.starts_with("D:")) continue;
					const std::string Name = Entry.substr(2);
					const auto Source = Output / std::filesystem::u8path(Name);
					if (auto Safe = CheckNoLinks(Source); !Safe) { Finish(RequestID, AssetExportPhase::Failed, Safe.error()); return; }
					auto Actual = Fingerprint(Source, CancellationToken);
					if (!Actual) { FinishPackageWorkError(Actual.error()); return; }
					if (Actual->DigestHex != ExpectedDigest)
						{ Finish(RequestID, AssetExportPhase::Failed, MakeError(AssetExportErrorCode::PublicationFailed,
							"An existing package file changed while the candidate was being staged", Source)); return; }
					if (auto Copied = CopyOne(Source, Candidate / std::filesystem::u8path(Name), CancellationToken); !Copied)
						{ FinishPackageWorkError(Copied.error()); return; }
					if (Cancelled()) { Finish(RequestID, AssetExportPhase::Cancelled,
						MakeError(AssetExportErrorCode::Cancelled, "Export cancelled after preserving an existing package file")); return; }
				}
			}
			auto CopiedOriginal = CaptureTree(Candidate, CancellationToken);
			if (!CopiedOriginal)
			{
				FinishPackageWorkError(CopiedOriginal.error());
				return;
			}
			if (*CopiedOriginal != CandidateExpected)
			{
				Finish(RequestID, AssetExportPhase::Failed, MakeError(AssetExportErrorCode::RecoveryRequired,
					"Candidate differs from the validated prior package copy", Candidate));
				return;
			}
			if (Cancelled()) { Finish(RequestID, AssetExportPhase::Cancelled,
				MakeError(AssetExportErrorCode::Cancelled, "Export cancelled after validating the prior package copy")); return; }
			Update(RequestID, AssetExportPhase::Stage, 0.14f, "Staging validated runtime template");
			for (const std::string& Relative : Descriptor->RuntimeFiles)
			{
				if (Cancelled()) { Finish(RequestID, AssetExportPhase::Cancelled, MakeError(AssetExportErrorCode::Cancelled, "Export cancelled during template staging")); return; }
				const auto Source = *Template / std::filesystem::u8path(Relative);
				if (auto Safe = CheckNoLinks(Source); !Safe) { Finish(RequestID, AssetExportPhase::Failed, Safe.error()); return; }
				const auto Status = std::filesystem::symlink_status(Source, Error);
				if (Error || !std::filesystem::is_regular_file(Status)) { Finish(RequestID, AssetExportPhase::Failed, MakeError(AssetExportErrorCode::TemplateIncomplete, "Required template file is missing or not regular", Source)); return; }
				if (std::filesystem::file_size(Source, Error) > MaximumExportFileBytes || Error) { Finish(RequestID, AssetExportPhase::Failed, MakeError(AssetExportErrorCode::TemplateIncomplete, "Template file exceeds the bounded size", Source)); return; }
				auto SourceDigest = Fingerprint(Source, CancellationToken);
				if (!SourceDigest) { FinishPackageWorkError(SourceDigest.error()); return; }
				if (Cancelled()) { Finish(RequestID, AssetExportPhase::Cancelled,
					MakeError(AssetExportErrorCode::Cancelled, "Export cancelled after validating a template file")); return; }
				const auto Destination = Candidate / std::filesystem::u8path(Relative);
				if (auto Copied = CopyOne(Source, Destination, CancellationToken); !Copied) { FinishPackageWorkError(Copied.error()); return; }
				AddParentDirectoryReceipts(CandidateExpected, Relative);
				CandidateExpected.insert_or_assign("F:" + Relative, SourceDigest->DigestHex);
				Recovery.CandidateTree = CandidateExpected;
				auto CopiedDigest = Fingerprint(Destination, CancellationToken);
				if (!CopiedDigest) { FinishPackageWorkError(CopiedDigest.error()); return; }
				if (CopiedDigest->DigestHex != SourceDigest->DigestHex)
					{ Finish(RequestID, AssetExportPhase::Failed, MakeError(AssetExportErrorCode::StageFailed,
						"Template bytes changed or could not be verified after copying", Destination)); return; }
				if (Cancelled()) { Finish(RequestID, AssetExportPhase::Cancelled,
					MakeError(AssetExportErrorCode::Cancelled, "Export cancelled after staging a template file")); return; }
			}
			auto TemplateCandidate = CaptureTree(Candidate, CancellationToken);
			if (!TemplateCandidate)
			{
				FinishPackageWorkError(TemplateCandidate.error());
				return;
			}
			if (*TemplateCandidate != CandidateExpected)
			{
				Finish(RequestID, AssetExportPhase::Failed, MakeError(AssetExportErrorCode::RecoveryRequired,
					"Candidate contains an entry outside the staged package receipts", Candidate));
				return;
			}
			if (auto Saved = SaveRecoveryRecord(JournalPath, Recovery); !Saved)
				{ Finish(RequestID, AssetExportPhase::Failed, Saved.error()); return; }
			if (Cancelled()) { Finish(RequestID, AssetExportPhase::Cancelled, MakeError(AssetExportErrorCode::Cancelled, "Export cancelled before cooking")); return; }
			Update(RequestID, AssetExportPhase::Cook, 0.25f, "Resolving and cooking the saved project content");
			AssetDependencyCatalogBuildOptions CatalogOptions;
			CatalogOptions.CancellationToken = CancellationToken;
			CatalogOptions.BeforeFinalInputValidation = Request.BeforeDependencyFinalValidation;
			auto Snapshot = AssetDependencyCatalogBuilder::Build(
				Project->GetRootPath(), Project->GetDescription().ID, 1, CatalogOptions);
			if (Cancelled())
				{ Finish(RequestID, AssetExportPhase::Cancelled, MakeError(AssetExportErrorCode::Cancelled,
					"Export cancelled during dependency analysis")); return; }
			if (!Snapshot) { Finish(RequestID, AssetExportPhase::Failed, MakeError(AssetExportErrorCode::DependencyResolutionFailed, Snapshot.error().Message)); return; }
			AssetCookRequest CookRequest;
			CookRequest.CancellationToken = CancellationToken;
			CookRequest.SourceSnapshot = std::move(*Snapshot);
			CookRequest.SourceProjectFile = Project->GetFilePath();
			CookRequest.StartupScene = Request.Profile.StartupScene;
			CookRequest.DynamicRoots = Request.Profile.DynamicRoots;
			CookRequest.Compatibility = { TargetPlatform(Request.Profile.TargetPlatform), "Vulkan", std::string(CookedRuntimeABIIdentifier), Request.Profile.Configuration };
			const std::filesystem::path ResourceRoot = Candidate / ResourceRelative;
			const std::filesystem::path CookStagingRoot = Candidate / CookStagingDirectoryName;
			CookRequest.OutputRoot = CookStagingRoot;
			if (Request.BeforeCook) Request.BeforeCook();
			if (Cancelled())
				{ Finish(RequestID, AssetExportPhase::Cancelled,
					MakeError(AssetExportErrorCode::Cancelled, "Export cancelled before cooking the resolved products")); return; }
			auto Cooked = AssetCookService::Cook(CookRequest);
			if (!Cooked)
			{
				if (Cooked.error().Code == AssetCookErrorCode::Cancelled || Cancelled())
					Finish(RequestID, AssetExportPhase::Cancelled, MakeError(AssetExportErrorCode::Cancelled,
						Cooked.error().Code == AssetCookErrorCode::Cancelled ? Cooked.error().Message : "Export cancelled during cooking",
						Cooked.error().Path));
				else Finish(RequestID, AssetExportPhase::Failed,
					MakeError(AssetExportErrorCode::CookFailed, Cooked.error().Message, Cooked.error().Path));
				return;
			}
			if (Cooked->StagingCleanupPending)
				{ Finish(RequestID, AssetExportPhase::Failed, MakeError(AssetExportErrorCode::CookFailed,
					"Cook retained private staging data; the export candidate was not published", Cooked->RetainedStagingPath)); return; }
			if (Cancelled())
				{ Finish(RequestID, AssetExportPhase::Cancelled, MakeError(AssetExportErrorCode::Cancelled,
					"Export cancelled after cooking")); return; }
			auto CookedFiles = EnumerateFiles(CookStagingRoot, CancellationToken);
			if (!CookedFiles) { FinishPackageWorkError(CookedFiles.error()); return; }
			if (Cancelled()) { Finish(RequestID, AssetExportPhase::Cancelled,
				MakeError(AssetExportErrorCode::Cancelled, "Export cancelled after enumerating cooked products")); return; }
			auto CookStagingTree = CaptureTree(CookStagingRoot, CancellationToken);
			if (!CookStagingTree) { FinishPackageWorkError(CookStagingTree.error()); return; }
			if (Cancelled()) { Finish(RequestID, AssetExportPhase::Cancelled,
				MakeError(AssetExportErrorCode::Cancelled, "Export cancelled after validating cooked products")); return; }
			std::map<std::string, std::string> CandidateWithCookStage = CandidateExpected;
			CandidateWithCookStage.insert_or_assign("D:" + std::string(CookStagingDirectoryName), "directory");
			for (const auto& [Name, Digest] : *CookStagingTree)
				CandidateWithCookStage.insert_or_assign(
					std::string(Name.starts_with("F:") ? "F:" : "D:") + std::string(CookStagingDirectoryName) + "/" + Name.substr(2), Digest);
			auto CandidateWithCookStageActual = CaptureTree(Candidate, CancellationToken);
			if (!CandidateWithCookStageActual)
			{
				FinishPackageWorkError(CandidateWithCookStageActual.error());
				return;
			}
			if (*CandidateWithCookStageActual != CandidateWithCookStage)
			{
				Finish(RequestID, AssetExportPhase::Failed, MakeError(AssetExportErrorCode::RecoveryRequired,
					"Cook staging contains an unrecorded candidate entry", Candidate));
				return;
			}
			CandidateExpected = std::move(CandidateWithCookStage);
			Recovery.CandidateTree = CandidateExpected;
			if (auto Saved = SaveRecoveryRecord(JournalPath, Recovery); !Saved)
				{ Finish(RequestID, AssetExportPhase::Failed, Saved.error()); return; }
			if (CookedFiles->size() + Descriptor->RuntimeFiles.size() + 1 > MaximumExportFileCount)
				{ Finish(RequestID, AssetExportPhase::Failed, MakeError(AssetExportErrorCode::VerificationFailed,
					"Template and cooked package file count exceeds its limit", Output)); return; }
			std::vector<ExportFileDestination> AllDestinations = InitialDestinations;
			AllDestinations.reserve(AllDestinations.size() + CookedFiles->size());
			for (const std::string& Name : *CookedFiles)
				AllDestinations.push_back({
					(ResourceRelative / "Content" / std::filesystem::u8path(Name)).generic_string(), ExportFileRole::Cooked });
			if (auto Preflight = PreflightDestinations(Output, ExistingTree, PreviousOwned, AllDestinations, OutputCaseSensitive); !Preflight)
				{ Finish(RequestID, AssetExportPhase::Failed, Preflight.error()); return; }
			for (const std::string& Name : *CookedFiles)
			{
				if (Cancelled()) { Finish(RequestID, AssetExportPhase::Cancelled,
					MakeError(AssetExportErrorCode::Cancelled, "Export cancelled while staging cooked products")); return; }
				const auto Source = CookStagingRoot / std::filesystem::u8path(Name);
				const auto Destination = ResourceRoot / "Content" / std::filesystem::u8path(Name);
				if (auto Safe = CheckNoLinks(Source); !Safe) { Finish(RequestID, AssetExportPhase::Failed, Safe.error()); return; }
				const std::string StagedName = "F:" + std::string(CookStagingDirectoryName) + "/" + Name;
				const auto StagedDigest = CandidateExpected.find(StagedName);
				if (StagedDigest == CandidateExpected.end())
					{ Finish(RequestID, AssetExportPhase::Failed, MakeError(AssetExportErrorCode::VerificationFailed,
						"Cook output file is absent from its staging receipt", Source)); return; }
				if (auto Copied = CopyOne(Source, Destination, CancellationToken); !Copied) { FinishPackageWorkError(Copied.error()); return; }
				const std::string RelativeDestination =
					(ResourceRelative / "Content" / std::filesystem::u8path(Name)).generic_string();
				AddParentDirectoryReceipts(CandidateExpected, RelativeDestination);
				CandidateExpected.insert_or_assign("F:" + RelativeDestination, StagedDigest->second);
				Recovery.CandidateTree = CandidateExpected;
				auto DestinationDigest = Fingerprint(Destination, CancellationToken);
				if (!DestinationDigest) { FinishPackageWorkError(DestinationDigest.error()); return; }
				if (DestinationDigest->DigestHex != StagedDigest->second)
					{ Finish(RequestID, AssetExportPhase::Failed, MakeError(AssetExportErrorCode::StageFailed,
						"Cooked product changed or could not be verified after staging", Destination)); return; }
				if (Cancelled()) { Finish(RequestID, AssetExportPhase::Cancelled,
					MakeError(AssetExportErrorCode::Cancelled, "Export cancelled after staging a cooked product")); return; }
			}
			Recovery.State = RecoveryStateStagingCleanupPending;
			if (auto Saved = SaveRecoveryRecord(JournalPath, Recovery); !Saved)
				{ Finish(RequestID, AssetExportPhase::Failed, Saved.error()); return; }
			if (auto Removed = RemoveValidatedTree(CookStagingRoot, *CookStagingTree, false); !Removed)
				{ Finish(RequestID, AssetExportPhase::Failed, Removed.error()); return; }
			std::erase_if(CandidateExpected, [](const auto& Entry)
			{
				return Entry.first == "D:" + std::string(CookStagingDirectoryName) ||
					Entry.first.starts_with("D:" + std::string(CookStagingDirectoryName) + "/") ||
					Entry.first.starts_with("F:" + std::string(CookStagingDirectoryName) + "/");
			});
			Recovery.State = RecoveryStateStaging;
			Recovery.CandidateTree = CandidateExpected;
			if (auto Saved = SaveRecoveryRecord(JournalPath, Recovery); !Saved)
				{ Finish(RequestID, AssetExportPhase::Failed, Saved.error()); return; }
			std::string RuntimeConfig = "project=" + Project->GetDescription().ID.ToString() + "\n";
			if (Request.Profile.Window)
				RuntimeConfig += "window=" + std::to_string(Request.Profile.Window->Width) + "x" +
					std::to_string(Request.Profile.Window->Height) + "\n";
			const std::string RuntimeConfigRelative = (ResourceRelative / RuntimeConfigurationName).generic_string();
			if (auto ConfigWrite = WriteText(Candidate / std::filesystem::u8path(RuntimeConfigRelative), RuntimeConfig); !ConfigWrite)
				{ Finish(RequestID, AssetExportPhase::Failed, ConfigWrite.error()); return; }
			AddParentDirectoryReceipts(CandidateExpected, RuntimeConfigRelative);
			CandidateExpected.insert_or_assign("F:" + RuntimeConfigRelative, FingerprintText(RuntimeConfig).DigestHex);
			Recovery.CandidateTree = CandidateExpected;
			if (Cancelled()) { Finish(RequestID, AssetExportPhase::Cancelled, MakeError(AssetExportErrorCode::Cancelled, "Export cancelled after cooking; previous package remains usable")); return; }
			Update(RequestID, AssetExportPhase::Verify, 0.82f, "Verifying package inventory and cooked manifest");
			auto Files = EnumerateFiles(Candidate, CancellationToken);
			if (!Files) { FinishPackageWorkError(Files.error()); return; }
			std::map<std::string, ExportFileRole> CurrentRoles;
			for (const std::string& Name : Descriptor->RuntimeFiles)
			{
				if (Cancelled()) { Finish(RequestID, AssetExportPhase::Cancelled,
					MakeError(AssetExportErrorCode::Cancelled, "Export cancelled while preparing package inventory")); return; }
				CurrentRoles.emplace(Name, ExportFileRole::Template);
			}
			for (const std::string& Name : *CookedFiles)
			{
				if (Cancelled()) { Finish(RequestID, AssetExportPhase::Cancelled,
					MakeError(AssetExportErrorCode::Cancelled, "Export cancelled while preparing package inventory")); return; }
				CurrentRoles.emplace((ResourceRelative / "Content" / std::filesystem::u8path(Name)).generic_string(), ExportFileRole::Cooked);
			}
			CurrentRoles.emplace((ResourceRelative / RuntimeConfigurationName).generic_string(), ExportFileRole::RuntimeConfiguration);
			CurrentRoles.emplace(std::string(TemplateOwnershipName), ExportFileRole::TemplateOwnership);
			std::set<std::string> CurrentOwned;
			for (const auto& [Name, Role] : CurrentRoles)
			{
				(void)Role;
				CurrentOwned.insert(Name);
			}
			for (const std::string& Stale : PreviousOwned)
			{
				if (Cancelled()) { Finish(RequestID, AssetExportPhase::Cancelled,
					MakeError(AssetExportErrorCode::Cancelled, "Export cancelled while removing stale owned package files")); return; }
				if (CurrentOwned.contains(Stale)) continue;
				const auto StalePath = Candidate / std::filesystem::u8path(Stale);
				if (auto Safe = CheckNoLinks(StalePath); !Safe) { Finish(RequestID, AssetExportPhase::Failed, Safe.error()); return; }
				const auto Expected = ExistingTree.find("F:" + Stale);
				auto Actual = Fingerprint(StalePath, CancellationToken);
				if (!Actual) { FinishPackageWorkError(Actual.error()); return; }
				if (Expected == ExistingTree.end() || Actual->DigestHex != Expected->second)
					{ Finish(RequestID, AssetExportPhase::Failed, MakeError(AssetExportErrorCode::PublicationFailed,
						"Stale owned file no longer matches the previous package receipt", StalePath)); return; }
				std::filesystem::remove(StalePath, Error);
				if (Error) { Finish(RequestID, AssetExportPhase::Failed, MakeError(AssetExportErrorCode::StageFailed,
					"Could not remove stale owned package file: " + Error.message(), StalePath)); return; }
				CandidateExpected.erase("F:" + Stale);
				Recovery.CandidateTree = CandidateExpected;
			}
			Json TemplateOwnership;
			TemplateOwnership["format"] = "PulseForgeTemplateOwnership";
			TemplateOwnership["version"] = 1;
			TemplateOwnership["profileId"] = Request.Profile.ProfileID.ToString();
			TemplateOwnership["projectId"] = Project->GetDescription().ID.ToString();
			TemplateOwnership["entrypoint"] = Descriptor->Entrypoint;
			TemplateOwnership["templateFiles"] = std::vector<std::string>(TemplateFiles.begin(), TemplateFiles.end());
			TemplateOwnership["membershipDigest"] = TemplateMembershipDigest(TemplateFiles, Descriptor->Entrypoint);
			const std::string TemplateOwnershipText = TemplateOwnership.dump(2) + "\n";
			if (auto Saved = WriteText(Candidate / std::filesystem::u8path(TemplateOwnershipName), TemplateOwnershipText); !Saved)
				{ Finish(RequestID, AssetExportPhase::Failed, Saved.error()); return; }
			CandidateExpected.insert_or_assign("F:" + std::string(TemplateOwnershipName), FingerprintText(TemplateOwnershipText).DigestHex);
			Recovery.CandidateTree = CandidateExpected;
			Json OwnedFiles = Json::object();
			for (const auto& [Name, Role] : CurrentRoles)
			{
				if (Cancelled()) { Finish(RequestID, AssetExportPhase::Cancelled,
					MakeError(AssetExportErrorCode::Cancelled, "Export cancelled while preparing package ownership receipts")); return; }
				auto Digest = Fingerprint(Candidate / std::filesystem::u8path(Name), CancellationToken);
				if (!Digest) { FinishPackageWorkError(Digest.error()); return; }
				Json Receipt;
				Receipt["role"] = std::string(RoleName(Role));
				Receipt["digest"] = Digest->DigestHex;
				OwnedFiles[Name] = std::move(Receipt);
			}
			Json Inventory;
			Inventory["format"] = "PulseForgeExportInventory";
			Inventory["version"] = 3;
			Inventory["profileId"] = Request.Profile.ProfileID.ToString();
			Inventory["projectId"] = Project->GetDescription().ID.ToString();
			Inventory["cookGeneration"] = Cooked->GenerationIdentity;
			Inventory["resourcePrefix"] = ResourcePrefix;
			Inventory["files"] = std::move(OwnedFiles);
			const std::string InventoryText = Inventory.dump(2) + "\n";
			if (auto Saved = WriteText(Candidate / InventoryName, InventoryText); !Saved) { Finish(RequestID, AssetExportPhase::Failed, Saved.error()); return; }
			CandidateExpected.insert_or_assign("F:" + std::string(InventoryName), FingerprintText(InventoryText).DigestHex);
			Recovery.CandidateTree = CandidateExpected;
			if (Cancelled()) { Finish(RequestID, AssetExportPhase::Cancelled, MakeError(AssetExportErrorCode::Cancelled, "Export cancelled before package publication")); return; }
			Update(RequestID, AssetExportPhase::Publish, 0.94f, "Publishing verified package");
			const bool OutputStillExists = std::filesystem::exists(Output, Error);
			if (Error) { Finish(RequestID, AssetExportPhase::Failed, MakeError(AssetExportErrorCode::PublicationFailed, "Could not revalidate destination: " + Error.message(), Output)); return; }
			if (OutputStillExists)
			{
				auto FreshTree = CaptureTree(Output, CancellationToken);
				if (!FreshTree)
				{
					FinishPackageWorkError(FreshTree.error());
					return;
				}
				if (*FreshTree != ExistingTree)
					{ Finish(RequestID, AssetExportPhase::Failed, MakeError(AssetExportErrorCode::PublicationFailed,
						"Existing package membership or contents changed after staging; preserving it", Output)); return; }
				auto FreshOwnership = ValidateInventory(Output, TemplateFiles, ResourcePrefix,
					Request.Profile.ProfileID.ToString(), Project->GetDescription().ID.ToString(), OutputCaseSensitive);
				if (!FreshOwnership || FreshOwnership->Files != PreviousOwned)
					{ Finish(RequestID, AssetExportPhase::Failed, FreshOwnership ?
						MakeError(AssetExportErrorCode::PublicationFailed, "Package ownership changed after staging", Output) : FreshOwnership.error()); return; }
			}
			else if (!ExistingTree.empty())
				{ Finish(RequestID, AssetExportPhase::Failed, MakeError(AssetExportErrorCode::PublicationFailed,
					"Existing package disappeared after staging; refusing to replace it", Output)); return; }
			auto CandidateReceipt = CaptureTree(Candidate, CancellationToken);
			if (!CandidateReceipt)
			{
				FinishPackageWorkError(CandidateReceipt.error());
				return;
			}
			if (*CandidateReceipt != CandidateExpected)
				{ Finish(RequestID, AssetExportPhase::Failed, MakeError(AssetExportErrorCode::RecoveryRequired,
					"Candidate differs from its incrementally recorded ownership receipts", Candidate)); return; }
			Recovery.HadPreviousOutput = OutputStillExists;
			Recovery.State = RecoveryStatePrepared;
			Recovery.PreviousTree = ExistingTree;
			Recovery.CandidateTree = CandidateExpected;
			if (auto Saved = SaveRecoveryRecord(JournalPath, Recovery); !Saved)
				{ Finish(RequestID, AssetExportPhase::Failed, Saved.error()); return; }
			if (!TryBeginPublication(RequestID))
				{ Finish(RequestID, AssetExportPhase::Cancelled,
					MakeError(AssetExportErrorCode::Cancelled, "Export cancelled before the package publication boundary")); return; }
			if (Request.BeforePublicationRename) Request.BeforePublicationRename();
			JournalCleanup.Keep = true;
			CandidateCleanup.Path.clear();
			if (OutputStillExists)
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
