#include "Core/PulseForgePCH.h"
#include "Assets/AssetOperations.h"

#include "Assets/AssetMetadata.h"

#include <fstream>
#include <iterator>
#include <limits>
#include <span>
#include <system_error>

#if defined(_WIN32)
	#include <Windows.h>
#endif

namespace PulseForge
{
	namespace
	{
		using FilesystemPath = std::filesystem::path;

		AssetOperationError MakeError(
			AssetOperationErrorCode Code,
			FilesystemPath Path,
			std::string Message)
		{
			return { Code, std::move(Path), std::move(Message) };
		}

		struct ProjectPaths
		{
			FilesystemPath Root;
		};

		std::expected<ProjectPaths, AssetOperationError> OpenProject(const FilesystemPath& ProjectRoot)
		{
			if (ProjectRoot.empty())
			{
				return std::unexpected(MakeError(
					AssetOperationErrorCode::InvalidProject,
					{},
					"Project root path must not be empty"));
			}

			std::error_code Error;
			const FilesystemPath AbsoluteRoot = std::filesystem::absolute(ProjectRoot, Error).lexically_normal();
			if (Error || !std::filesystem::is_directory(AbsoluteRoot, Error) || Error)
			{
				return std::unexpected(MakeError(
					AssetOperationErrorCode::InvalidProject,
					ProjectRoot,
					"Project root is not an accessible directory: " + ProjectRoot.generic_string()));
			}

			const FilesystemPath Assets = AbsoluteRoot / "Assets";
			Error.clear();
			const auto AssetsStatus = std::filesystem::symlink_status(Assets, Error);
			if (Error || std::filesystem::is_symlink(AssetsStatus) || !std::filesystem::is_directory(AssetsStatus))
			{
				return std::unexpected(MakeError(
					AssetOperationErrorCode::InvalidProject,
					"Assets",
					"Project requires a regular, non-symlink Assets directory"));
			}

			return ProjectPaths{ AbsoluteRoot };
		}

		std::expected<void, AssetOperationError> ValidateProjectRegistry(
			AssetRegistry& Registry,
			const FilesystemPath& ProjectRoot,
			const AssetOperations::RegistryRefresh& Refresh)
		{
			(void)Registry;
			const auto RebuildResult = Refresh(ProjectRoot);
			if (RebuildResult)
				return {};

			std::string Message = "Asset operation refused because the project registry contains errors";
			FilesystemPath Path;
			for (const AssetRegistryIssue& Issue : RebuildResult.error().Issues)
			{
				if (Path.empty())
					Path = Issue.ProjectRelativePath;
				Message += "\n - " + Issue.Message;
			}
			return std::unexpected(MakeError(
				AssetOperationErrorCode::InvalidProjectAssets,
				std::move(Path),
				std::move(Message)));
		}

		std::expected<void, AssetOperationError> ValidateProjectRegistry(
			AssetRegistry& Registry,
			const FilesystemPath& ProjectRoot)
		{
			return ValidateProjectRegistry(Registry, ProjectRoot, [&Registry](const FilesystemPath& Root)
			{
				return Registry.Rebuild(Root);
			});
		}

		AssetOperationError MakeRegistryRefreshError(
			const AssetOperationError& RegistryError,
			std::string Operation)
		{
			return MakeError(
				AssetOperationErrorCode::RegistryRefreshFailed,
				RegistryError.Path,
				std::move(Operation) + " completed, but the asset registry could not be refreshed: " + RegistryError.Message);
		}

		bool IsValidProjectAssetPath(const FilesystemPath& Path)
		{
			if (Path.empty() || Path.is_absolute() || Path.has_root_name() || Path.has_root_directory() ||
				Path.lexically_normal() != Path || Path.extension() == ".meta")
			{
				return false;
			}

			bool IsFirstComponent = true;
			size_t ComponentCount = 0;
			for (const FilesystemPath& Component : Path)
			{
				if (Component.empty() || Component == "." || Component == "..")
					return false;
				if (IsFirstComponent && Component != "Assets")
					return false;
				IsFirstComponent = false;
				++ComponentCount;
			}
			return ComponentCount >= 2 && !Path.filename().empty();
		}

		bool IsValidProjectFolderPath(const FilesystemPath& Path, bool AllowAssetsRoot)
		{
			if (Path.empty() || Path.is_absolute() || Path.has_root_name() || Path.has_root_directory() ||
				Path.lexically_normal() != Path)
			{
				return false;
			}

			bool IsFirstComponent = true;
			size_t ComponentCount = 0;
			for (const FilesystemPath& Component : Path)
			{
				if (Component.empty() || Component == "." || Component == ".." || Component == ".meta")
					return false;
				if (IsFirstComponent && Component != "Assets")
					return false;
				IsFirstComponent = false;
				++ComponentCount;
			}
			return ComponentCount >= (AllowAssetsRoot ? 1u : 2u) && !Path.filename().empty();
		}

		bool IsReparsePoint(const FilesystemPath& Path)
		{
#if defined(_WIN32)
			const DWORD Attributes = GetFileAttributesW(Path.c_str());
			return Attributes != INVALID_FILE_ATTRIBUTES && (Attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
			(void)Path;
			return false;
#endif
		}

		bool IsCaseInsensitivePathPrefix(const FilesystemPath& Prefix, const FilesystemPath& Path)
		{
			auto PrefixComponent = Prefix.begin();
			auto PathComponent = Path.begin();
			for (; PrefixComponent != Prefix.end(); ++PrefixComponent, ++PathComponent)
			{
				if (PathComponent == Path.end())
					return false;
#if defined(_WIN32)
				if (CompareStringOrdinal(
					PrefixComponent->c_str(), static_cast<int>(PrefixComponent->native().size()),
					PathComponent->c_str(), static_cast<int>(PathComponent->native().size()), TRUE) != CSTR_EQUAL)
				{
					return false;
				}
#else
				if (*PrefixComponent != *PathComponent)
					return false;
#endif
			}
			return true;
		}

		bool AreCaseInsensitivePathComponentsEqual(const FilesystemPath& Left, const FilesystemPath& Right)
		{
			auto LeftComponent = Left.begin();
			auto RightComponent = Right.begin();
			for (; LeftComponent != Left.end() && RightComponent != Right.end(); ++LeftComponent, ++RightComponent)
			{
#if defined(_WIN32)
				if (CompareStringOrdinal(
					LeftComponent->c_str(), static_cast<int>(LeftComponent->native().size()),
					RightComponent->c_str(), static_cast<int>(RightComponent->native().size()), TRUE) != CSTR_EQUAL)
				{
					return false;
				}
#else
				if (*LeftComponent != *RightComponent)
					return false;
#endif
			}
			return LeftComponent == Left.end() && RightComponent == Right.end();
		}

		std::expected<AssetRecord, AssetOperationError> RenameCaseOnlyAsset(
			AssetRegistry& Registry,
			const ProjectPaths& Project,
			const AssetID& Asset,
			const FilesystemPath& SourcePath,
			const FilesystemPath& DestinationPath,
			const FilesystemPath& Source,
			const FilesystemPath& Destination,
			const FilesystemPath& SourceSidecar,
			const FilesystemPath& DestinationSidecar)
		{
			const auto TransactionID = UUID::Generate();
			if (!TransactionID)
			{
				return std::unexpected(MakeError(
					AssetOperationErrorCode::FilesystemFailure,
					SourcePath,
					"Could not create a unique temporary name for the case-only asset rename"));
			}

			const FilesystemPath StagingAsset = Source.parent_path() /
				(Source.filename().native() + std::filesystem::path(L".pulseforge-move-").native() +
					std::filesystem::path(TransactionID->ToString()).native());
			const FilesystemPath StagingSidecar = AssetMetadataSerializer::GetSidecarPath(StagingAsset);
			std::error_code Error;
			if (std::filesystem::exists(StagingAsset, Error) || Error || std::filesystem::exists(StagingSidecar, Error) || Error)
			{
				return std::unexpected(MakeError(
					AssetOperationErrorCode::FilesystemFailure,
					SourcePath,
					"Could not reserve unique temporary paths for the case-only asset rename"));
			}

			std::filesystem::rename(Source, StagingAsset, Error);
			if (Error)
			{
				return std::unexpected(MakeError(
					AssetOperationErrorCode::FilesystemFailure,
					SourcePath,
					"Could not stage the asset for a case-only rename: " + Error.message()));
			}

			Error.clear();
			std::filesystem::rename(SourceSidecar, StagingSidecar, Error);
			if (Error)
			{
				const std::string Failure = Error.message();
				Error.clear();
				std::filesystem::rename(StagingAsset, Source, Error);
				if (Error)
				{
					return std::unexpected(MakeError(
						AssetOperationErrorCode::RecoveryRequired,
						SourcePath,
						"Could not stage the asset sidecar (" + Failure + ") and could not restore the asset. Recovery path: " +
							StagingAsset.generic_string() + "; restore failure: " + Error.message()));
				}
				return std::unexpected(MakeError(
					AssetOperationErrorCode::FilesystemFailure,
					SourcePath,
					"Could not stage the sidecar for a case-only rename: " + Failure));
			}

			const auto RestoreStagedPair = [&]() -> std::string
			{
				std::error_code RestoreError;
				if (std::filesystem::exists(Destination, RestoreError) && !RestoreError)
					std::filesystem::rename(Destination, StagingAsset, RestoreError);
				if (RestoreError)
					return "asset staging restore failed: " + RestoreError.message();
				RestoreError.clear();
				if (std::filesystem::exists(DestinationSidecar, RestoreError) && !RestoreError)
					std::filesystem::rename(DestinationSidecar, StagingSidecar, RestoreError);
				if (RestoreError)
					return "sidecar staging restore failed: " + RestoreError.message();
				RestoreError.clear();
				std::filesystem::rename(StagingAsset, Source, RestoreError);
				if (RestoreError)
					return "asset source restore failed: " + RestoreError.message();
				RestoreError.clear();
				std::filesystem::rename(StagingSidecar, SourceSidecar, RestoreError);
				if (RestoreError)
					return "sidecar source restore failed: " + RestoreError.message();
				return {};
			};

			Error.clear();
			std::filesystem::rename(StagingAsset, Destination, Error);
			if (!Error)
			{
				Error.clear();
				std::filesystem::rename(StagingSidecar, DestinationSidecar, Error);
			}
			if (Error)
			{
				const std::string Failure = Error.message();
				const std::string RestoreFailure = RestoreStagedPair();
				if (!RestoreFailure.empty())
				{
					return std::unexpected(MakeError(
						AssetOperationErrorCode::RecoveryRequired,
						DestinationPath,
						"Case-only rename failed (" + Failure + ") and rollback failed: " + RestoreFailure + ". Recovery path: " +
							StagingAsset.generic_string()));
				}
				return std::unexpected(MakeError(
					AssetOperationErrorCode::FilesystemFailure,
					DestinationPath,
					"Could not complete the case-only rename: " + Failure));
			}

			if (auto RegistryResult = ValidateProjectRegistry(Registry, Project.Root); !RegistryResult)
			{
				const std::string RefreshFailure = RegistryResult.error().Message;
				const std::string RestoreFailure = RestoreStagedPair();
				if (RestoreFailure.empty())
				{
					if (auto RollbackRefresh = ValidateProjectRegistry(Registry, Project.Root); !RollbackRefresh)
					{
						return std::unexpected(MakeError(
							AssetOperationErrorCode::RecoveryRequired,
							SourcePath,
							"Case-only rename was rolled back, but the source registry could not be restored: " +
								RollbackRefresh.error().Message));
					}
				}
				else
				{
					return std::unexpected(MakeError(
						AssetOperationErrorCode::RecoveryRequired,
						DestinationPath,
						"Registry refresh failed (" + RefreshFailure + ") and rollback failed: " + RestoreFailure));
				}
				return std::unexpected(MakeRegistryRefreshError(RegistryResult.error(), "Case-only asset rename"));
			}

			return AssetRecord{ Asset, DestinationPath };
		}

		std::expected<void, AssetOperationError> ValidateManagedDirectory(
			const ProjectPaths& Project,
			const FilesystemPath& ProjectRelativePath,
			bool AllowAssetsRoot)
		{
			if (!IsValidProjectFolderPath(ProjectRelativePath, AllowAssetsRoot))
			{
				return std::unexpected(MakeError(
					AssetOperationErrorCode::InvalidPath,
					ProjectRelativePath,
					"Folder path must be a normalized project-relative directory under Assets"));
			}

			FilesystemPath Current = Project.Root;
			for (const FilesystemPath& Component : ProjectRelativePath)
			{
				Current /= Component;
				std::error_code Error;
				const auto Status = std::filesystem::symlink_status(Current, Error);
				if (Error || !std::filesystem::is_directory(Status) || std::filesystem::is_symlink(Status) ||
					IsReparsePoint(Current))
				{
					return std::unexpected(MakeError(
						AssetOperationErrorCode::InvalidPath,
						ProjectRelativePath,
						"Folder move paths must contain existing regular directories and no symbolic links or reparse points: " +
							Current.generic_string()));
				}
			}
			return {};
		}

		std::expected<void, AssetOperationError> ValidateNoReparseEntries(
			const FilesystemPath& AbsoluteFolder,
			const FilesystemPath& ProjectRelativePath)
		{
			std::error_code Error;
			std::filesystem::recursive_directory_iterator Iterator(AbsoluteFolder, Error);
			const std::filesystem::recursive_directory_iterator End;
			if (Error)
			{
				return std::unexpected(MakeError(
					AssetOperationErrorCode::FilesystemFailure,
					ProjectRelativePath,
					"Could not inspect folder contents before moving: " + Error.message()));
			}

			while (Iterator != End)
			{
				const FilesystemPath EntryPath = Iterator->path();
				const auto Status = Iterator->symlink_status(Error);
				if (Error || std::filesystem::is_symlink(Status) || IsReparsePoint(EntryPath))
				{
					Iterator.disable_recursion_pending();
					return std::unexpected(MakeError(
						AssetOperationErrorCode::InvalidPath,
						EntryPath.lexically_relative(AbsoluteFolder).empty()
							? ProjectRelativePath
							: ProjectRelativePath / EntryPath.lexically_relative(AbsoluteFolder),
						"Folder moves do not traverse or carry symbolic links or reparse points"));
				}
				Iterator.increment(Error);
				if (Error)
				{
					return std::unexpected(MakeError(
						AssetOperationErrorCode::FilesystemFailure,
						ProjectRelativePath,
						"Could not continue inspecting folder contents: " + Error.message()));
				}
			}
			return {};
		}

		std::expected<FilesystemPath, AssetOperationError> ResolveAssetPath(
			const ProjectPaths& Project,
			const FilesystemPath& ProjectRelativePath,
			bool MustExist)
		{
			if (!IsValidProjectAssetPath(ProjectRelativePath))
			{
				return std::unexpected(MakeError(
					AssetOperationErrorCode::InvalidPath,
					ProjectRelativePath,
					"Asset path must be a project-relative file below Assets and must not address a .meta sidecar"));
			}

			FilesystemPath Current = Project.Root;
			auto Component = ProjectRelativePath.begin();
			for (; Component != ProjectRelativePath.end(); ++Component)
			{
				Current /= *Component;
				const bool IsFinal = std::next(Component) == ProjectRelativePath.end();
				if (IsFinal)
					break;

				std::error_code Error;
				const auto Status = std::filesystem::symlink_status(Current, Error);
				if (Error || std::filesystem::is_symlink(Status) || !std::filesystem::is_directory(Status))
				{
					return std::unexpected(MakeError(
						AssetOperationErrorCode::InvalidPath,
						ProjectRelativePath,
						"Asset path parent must be an existing, non-symlink directory: " + Current.generic_string()));
				}
			}

			std::error_code Error;
			const auto Status = std::filesystem::symlink_status(Current, Error);
			const bool Exists = !Error && Status.type() != std::filesystem::file_type::not_found;
			if (Error && Error != std::errc::no_such_file_or_directory)
			{
				return std::unexpected(MakeError(
					AssetOperationErrorCode::FilesystemFailure,
					ProjectRelativePath,
					"Could not inspect asset path: " + Error.message()));
			}
			if (MustExist && (!Exists || std::filesystem::is_symlink(Status) || !std::filesystem::is_regular_file(Status)))
			{
				return std::unexpected(MakeError(
					AssetOperationErrorCode::SourceNotManaged,
					ProjectRelativePath,
					"Asset source is missing or is not a regular, non-symlink file: " + ProjectRelativePath.generic_string()));
			}
			if (!MustExist && Exists)
			{
				return std::unexpected(MakeError(
					AssetOperationErrorCode::DestinationExists,
					ProjectRelativePath,
					"Asset destination already exists: " + ProjectRelativePath.generic_string()));
			}
			return Current;
		}

		std::expected<void, AssetOperationError> RequireAbsent(
			const FilesystemPath& Path,
			const FilesystemPath& ProjectRelativePath)
		{
			std::error_code Error;
			const auto Status = std::filesystem::symlink_status(Path, Error);
			if (Error && Error != std::errc::no_such_file_or_directory)
			{
				return std::unexpected(MakeError(
					AssetOperationErrorCode::FilesystemFailure,
					ProjectRelativePath,
					"Could not inspect destination sidecar: " + Error.message()));
			}
			if (!Error && Status.type() != std::filesystem::file_type::not_found)
			{
				return std::unexpected(MakeError(
					AssetOperationErrorCode::DestinationExists,
					ProjectRelativePath,
					"Destination sidecar already exists: " + ProjectRelativePath.generic_string()));
			}
			return {};
		}

		std::expected<AssetMetadata, AssetOperationError> LoadManagedMetadata(
			const FilesystemPath& SourcePath,
			const FilesystemPath& ProjectRelativePath)
		{
			const FilesystemPath SidecarPath = AssetMetadataSerializer::GetSidecarPath(SourcePath);
			std::error_code Error;
			const auto Status = std::filesystem::symlink_status(SidecarPath, Error);
			if (Error || std::filesystem::is_symlink(Status) || !std::filesystem::is_regular_file(Status))
			{
				return std::unexpected(MakeError(
					AssetOperationErrorCode::InvalidProjectAssets,
					ProjectRelativePath,
					"Managed asset sidecar is missing or is not a regular, non-symlink file"));
			}

			const auto Metadata = AssetMetadataSerializer::LoadFromFile(SidecarPath);
			if (!Metadata)
			{
				return std::unexpected(MakeError(
					AssetOperationErrorCode::InvalidProjectAssets,
					ProjectRelativePath,
					Metadata.error().Message));
			}
			return *Metadata;
		}

		bool RemoveFile(const FilesystemPath& Path, std::string& FailureMessage)
		{
			std::error_code Error;
			const bool Removed = std::filesystem::remove(Path, Error);
			if (Error || !Removed)
			{
				FailureMessage = Error ? Error.message() : "file was not removed";
				return false;
			}
			return true;
		}

		bool RemoveFileIfPresent(const FilesystemPath& Path, std::string& FailureMessage)
		{
			std::error_code Error;
			const auto Status = std::filesystem::symlink_status(Path, Error);
			if (Error == std::errc::no_such_file_or_directory ||
				(!Error && Status.type() == std::filesystem::file_type::not_found))
			{
				return true;
			}
			if (Error)
			{
				FailureMessage = Error.message();
				return false;
			}
			return RemoveFile(Path, FailureMessage);
		}

		bool RemoveTransactionDirectory(const FilesystemPath& Path, std::string& FailureMessage)
		{
			std::error_code Error;
			std::filesystem::remove_all(Path, Error);
			if (Error)
			{
				FailureMessage = Error.message();
				return false;
			}
			return true;
		}

		struct TransactionDirectoryGuard
		{
			FilesystemPath Path;

			~TransactionDirectoryGuard()
			{
				// Keep a best-effort fallback for exits that bypass explicit cleanup (for example, exceptions).
				if (!Path.empty())
				{
					std::error_code Error;
					std::filesystem::remove_all(Path, Error);
				}
			}

			bool Remove(std::string& FailureMessage)
			{
				if (Path.empty())
					return true;
				if (!RemoveTransactionDirectory(Path, FailureMessage))
				{
					// An explicit failure is reported to the caller. Relinquish ownership so destruction
					// cannot silently retry and make the reported recovery state inaccurate.
					Path.clear();
					return false;
				}
				Path.clear();
				return true;
			}
		};

		bool RemoveDestinationPair(const FilesystemPath& SourcePath, std::string& FailureMessage)
		{
			std::string SidecarFailure;
			const bool SidecarRemoved = RemoveFile(AssetMetadataSerializer::GetSidecarPath(SourcePath), SidecarFailure);
			std::string SourceFailure;
			const bool SourceRemoved = RemoveFile(SourcePath, SourceFailure);
			if (!SidecarRemoved || !SourceRemoved)
			{
				FailureMessage = "Could not roll back destination asset";
				if (!SidecarRemoved)
					FailureMessage += "; sidecar: " + SidecarFailure;
				if (!SourceRemoved)
					FailureMessage += "; source: " + SourceFailure;
				return false;
			}
			return true;
		}

		AssetOperationError MakeMetadataOperationError(
			const AssetMetadataError& MetadataError,
			const FilesystemPath& RelativePath)
		{
			return MakeError(
				AssetOperationErrorCode::MetadataFailure,
				RelativePath,
				MetadataError.Message);
		}
	}

	std::expected<AssetRecord, AssetOperationError> AssetOperations::ImportFile(
		AssetRegistry& Registry,
		const std::filesystem::path& ProjectRoot,
		const std::filesystem::path& SourceFile,
		const std::filesystem::path& DestinationPath)
	{
		const auto Project = OpenProject(ProjectRoot);
		if (!Project)
			return std::unexpected(Project.error());
		if (auto RegistryResult = ValidateProjectRegistry(Registry, Project->Root); !RegistryResult)
			return std::unexpected(RegistryResult.error());

		if (SourceFile.empty())
		{
			return std::unexpected(MakeError(
				AssetOperationErrorCode::ImportSourceInvalid,
				SourceFile,
				"Import source path must not be empty"));
		}

		std::error_code Error;
		const auto SourceStatus = std::filesystem::symlink_status(SourceFile, Error);
		if (Error || std::filesystem::is_symlink(SourceStatus) || !std::filesystem::is_regular_file(SourceStatus))
		{
			return std::unexpected(MakeError(
				AssetOperationErrorCode::ImportSourceInvalid,
				SourceFile,
				"Import source must be an accessible regular, non-symlink file"));
		}

		const auto Destination = ResolveAssetPath(*Project, DestinationPath, false);
		if (!Destination)
			return std::unexpected(Destination.error());
		if (auto SidecarResult = RequireAbsent(AssetMetadataSerializer::GetSidecarPath(*Destination), DestinationPath);
			!SidecarResult)
		{
			return std::unexpected(SidecarResult.error());
		}

		Error.clear();
		if (!std::filesystem::copy_file(SourceFile, *Destination, std::filesystem::copy_options::none, Error) || Error)
		{
			const std::string CopyFailure = Error ? Error.message() : "destination already exists";
			if (Error == std::errc::file_exists)
			{
				return std::unexpected(MakeError(
					AssetOperationErrorCode::DestinationExists,
					DestinationPath,
					"Asset destination appeared during import and was not overwritten"));
			}

			std::string CleanupFailure;
			if (!RemoveFileIfPresent(*Destination, CleanupFailure))
			{
				return std::unexpected(MakeError(
					AssetOperationErrorCode::RecoveryRequired,
					DestinationPath,
					"Asset import failed (" + CopyFailure + ") and partial output could not be removed: " + CleanupFailure));
			}
			return std::unexpected(MakeError(
				AssetOperationErrorCode::FilesystemFailure,
				DestinationPath,
				"Could not copy import source: " + CopyFailure));
		}

		const auto Metadata = AssetMetadataSerializer::CreateForNewAsset(*Destination);
		if (!Metadata)
		{
			std::string SourceCleanupFailure;
			const bool SourceRemoved = RemoveFile(*Destination, SourceCleanupFailure);
			std::string SidecarCleanupFailure;
			const bool SidecarRemoved = Metadata.error().Code == AssetMetadataErrorCode::MetadataAlreadyExists ||
				RemoveFileIfPresent(AssetMetadataSerializer::GetSidecarPath(*Destination), SidecarCleanupFailure);
			if (!SourceRemoved || !SidecarRemoved)
			{
				return std::unexpected(MakeError(
					AssetOperationErrorCode::RecoveryRequired,
					DestinationPath,
					"Could not create imported asset metadata (" + Metadata.error().Message +
					") and rollback failed. Source cleanup: " + SourceCleanupFailure +
					"; sidecar cleanup: " + SidecarCleanupFailure));
			}
			return std::unexpected(MakeMetadataOperationError(Metadata.error(), DestinationPath));
		}

		if (auto RegistryResult = ValidateProjectRegistry(Registry, Project->Root); !RegistryResult)
		{
			std::string RollbackFailure;
			if (!RemoveDestinationPair(*Destination, RollbackFailure))
			{
				return std::unexpected(MakeError(
					AssetOperationErrorCode::RecoveryRequired,
					DestinationPath,
					"Imported asset failed registry validation (" + RegistryResult.error().Message +
					") and rollback failed: " + RollbackFailure));
			}
			if (auto RefreshResult = ValidateProjectRegistry(Registry, Project->Root); !RefreshResult)
				return std::unexpected(MakeRegistryRefreshError(RefreshResult.error(), "Import rollback"));
			return std::unexpected(RegistryResult.error());
		}

		const auto ImportedRecord = Registry.Find(Metadata->ID);
		if (!ImportedRecord)
		{
			std::string RollbackFailure;
			if (!RemoveDestinationPair(*Destination, RollbackFailure))
			{
				return std::unexpected(MakeError(
					AssetOperationErrorCode::RecoveryRequired,
					DestinationPath,
					"Imported asset is missing from the rebuilt registry and rollback failed: " + RollbackFailure));
			}
			if (auto RefreshResult = ValidateProjectRegistry(Registry, Project->Root); !RefreshResult)
				return std::unexpected(MakeRegistryRefreshError(RefreshResult.error(), "Import rollback"));
			return std::unexpected(MakeError(
				AssetOperationErrorCode::RegistryRefreshFailed,
				DestinationPath,
				"Imported asset UUID was not present after registry reconstruction"));
		}
		return *ImportedRecord;
	}

	std::expected<AssetRecord, AssetOperationError> AssetOperations::CreateAssetFromBytes(
		AssetRegistry& Registry,
		const std::filesystem::path& ProjectRoot,
		std::span<const std::byte> Contents,
		const std::filesystem::path& DestinationPath)
	{
		const auto Project = OpenProject(ProjectRoot);
		if (!Project)
			return std::unexpected(Project.error());
		if (auto RegistryResult = ValidateProjectRegistry(Registry, Project->Root); !RegistryResult)
			return std::unexpected(RegistryResult.error());

		const auto Destination = ResolveAssetPath(*Project, DestinationPath, false);
		if (!Destination)
			return std::unexpected(Destination.error());
		if (auto SidecarResult = RequireAbsent(AssetMetadataSerializer::GetSidecarPath(*Destination), DestinationPath);
			!SidecarResult)
		{
			return std::unexpected(SidecarResult.error());
		}

		if (Contents.size() > static_cast<size_t>(std::numeric_limits<std::streamsize>::max()))
		{
			return std::unexpected(MakeError(
				AssetOperationErrorCode::FilesystemFailure,
				DestinationPath,
				"New asset content exceeds the filesystem stream size limit"));
		}

		std::error_code Error;
		const FilesystemPath TemporaryRoot = std::filesystem::temp_directory_path(Error);
		if (Error)
		{
			return std::unexpected(MakeError(
				AssetOperationErrorCode::FilesystemFailure,
				DestinationPath,
				"Could not resolve a temporary directory for managed asset creation: " + Error.message()));
		}

		FilesystemPath TemporaryDirectory;
		for (size_t Attempt = 0; Attempt < 8; ++Attempt)
		{
			const auto Identifier = UUID::Generate();
			if (!Identifier)
			{
				return std::unexpected(MakeError(
					AssetOperationErrorCode::FilesystemFailure,
					DestinationPath,
					Identifier.error().Message));
			}

			TemporaryDirectory = TemporaryRoot / ("PulseForgeAssetCreate-" + Identifier->ToString());
			Error.clear();
			if (std::filesystem::create_directory(TemporaryDirectory, Error))
				break;
			if (Error && Error != std::errc::file_exists)
			{
				return std::unexpected(MakeError(
					AssetOperationErrorCode::FilesystemFailure,
					DestinationPath,
					"Could not create a temporary managed-asset transaction directory: " + Error.message()));
			}
			TemporaryDirectory.clear();
		}
		if (TemporaryDirectory.empty())
		{
			return std::unexpected(MakeError(
				AssetOperationErrorCode::FilesystemFailure,
				DestinationPath,
				"Could not reserve a unique temporary directory for managed asset creation"));
		}

		TransactionDirectoryGuard TemporaryDirectoryOwner{ TemporaryDirectory };
		const FilesystemPath TemporarySource = TemporaryDirectory / "source.asset";
		std::ofstream Output(TemporarySource, std::ios::binary | std::ios::out | std::ios::trunc);
		if (!Output.is_open())
		{
			std::string CleanupFailure;
			if (!TemporaryDirectoryOwner.Remove(CleanupFailure))
			{
				AssetOperationError RecoveryError = MakeError(
					AssetOperationErrorCode::RecoveryRequired,
					DestinationPath,
					"Could not open temporary source data and temporary cleanup failed: " + CleanupFailure);
				RecoveryError.RecoveryPath = TemporaryDirectory;
				return std::unexpected(std::move(RecoveryError));
			}
			return std::unexpected(MakeError(
				AssetOperationErrorCode::FilesystemFailure,
				DestinationPath,
				"Could not open a temporary source file for managed asset creation"));
		}

		if (!Contents.empty())
			Output.write(reinterpret_cast<const char*>(Contents.data()), static_cast<std::streamsize>(Contents.size()));
		Output.flush();
		const bool ContentsWritten = static_cast<bool>(Output);
		Output.close();
		if (!ContentsWritten || Output.fail())
		{
			std::string CleanupFailure;
			if (!TemporaryDirectoryOwner.Remove(CleanupFailure))
			{
				AssetOperationError RecoveryError = MakeError(
					AssetOperationErrorCode::RecoveryRequired,
					DestinationPath,
					"Could not write temporary source data and temporary cleanup failed: " + CleanupFailure);
				RecoveryError.RecoveryPath = TemporaryDirectory;
				return std::unexpected(std::move(RecoveryError));
			}
			return std::unexpected(MakeError(
				AssetOperationErrorCode::FilesystemFailure,
				DestinationPath,
				"Could not write temporary source data for managed asset creation"));
		}

		auto CreatedAsset = ImportFile(Registry, Project->Root, TemporarySource, DestinationPath);
		std::string CleanupFailure;
		if (!TemporaryDirectoryOwner.Remove(CleanupFailure))
		{
			if (CreatedAsset)
			{
				AssetOperationError CleanupError = MakeError(
					AssetOperationErrorCode::CommittedWithCleanupFailure,
					DestinationPath,
					"Managed asset creation committed, but temporary source cleanup failed at " +
					TemporaryDirectory.generic_string() + ": " + CleanupFailure);
				CleanupError.CommittedAsset = *CreatedAsset;
				CleanupError.RecoveryPath = TemporaryDirectory;
				return std::unexpected(std::move(CleanupError));
			}

			AssetOperationError RecoveryError = MakeError(
				AssetOperationErrorCode::RecoveryRequired,
				DestinationPath,
				"Managed asset import failed (" + CreatedAsset.error().Message +
				") and temporary source cleanup failed at " + TemporaryDirectory.generic_string() + ": " + CleanupFailure);
			RecoveryError.RecoveryPath = TemporaryDirectory;
			return std::unexpected(std::move(RecoveryError));
		}
		if (!CreatedAsset)
			return std::unexpected(std::move(CreatedAsset.error()));
		return *CreatedAsset;
	}

	std::expected<AssetRecord, AssetOperationError> AssetOperations::Move(
		AssetRegistry& Registry,
		const std::filesystem::path& ProjectRoot,
		const AssetID& SourceAsset,
		const std::filesystem::path& DestinationPath)
	{
		return MoveImpl(Registry, ProjectRoot, SourceAsset, DestinationPath, [&Registry](const auto& Root)
		{
			return Registry.Rebuild(Root);
		});
	}

#if defined(PF_DEBUG)
	std::expected<AssetRecord, AssetOperationError> AssetOperations::MoveWithRegistryRefreshForTesting(
		AssetRegistry& Registry,
		const std::filesystem::path& ProjectRoot,
		const AssetID& SourceAsset,
		const std::filesystem::path& DestinationPath,
		const RegistryRefresh& Refresh)
	{
		if (!Refresh)
			return std::unexpected(MakeError(AssetOperationErrorCode::FilesystemFailure, DestinationPath,
				"The test registry refresh callback is empty"));
		return MoveImpl(Registry, ProjectRoot, SourceAsset, DestinationPath, Refresh);
	}
#endif

	std::expected<AssetRecord, AssetOperationError> AssetOperations::MoveImpl(
		AssetRegistry& Registry,
		const std::filesystem::path& ProjectRoot,
		const AssetID& SourceAsset,
		const std::filesystem::path& DestinationPath,
		const RegistryRefresh& Refresh)
	{
		const auto Project = OpenProject(ProjectRoot);
		if (!Project)
			return std::unexpected(Project.error());
		if (auto RegistryResult = ValidateProjectRegistry(Registry, Project->Root, Refresh); !RegistryResult)
			return std::unexpected(RegistryResult.error());
		const auto SourceRecord = Registry.Find(SourceAsset);
		if (!SourceRecord)
		{
			return std::unexpected(MakeError(
				AssetOperationErrorCode::AssetNotFound,
				{},
				"Asset UUID " + SourceAsset.ToString() + " is not present in the project registry"));
		}
		const FilesystemPath SourcePath = SourceRecord->ProjectRelativePath;
		if (SourcePath == DestinationPath)
		{
			return std::unexpected(MakeError(
				AssetOperationErrorCode::InvalidPath,
				SourcePath,
				"Asset source and destination paths must differ"));
		}

		const auto Source = ResolveAssetPath(*Project, SourcePath, true);
		if (!Source)
			return std::unexpected(Source.error());
		const auto Destination = ResolveAssetPath(*Project, DestinationPath, false);
		const FilesystemPath AbsoluteDestination = Project->Root / DestinationPath;
		const FilesystemPath SourceSidecar = AssetMetadataSerializer::GetSidecarPath(*Source);
		const FilesystemPath DestinationSidecar = AssetMetadataSerializer::GetSidecarPath(AbsoluteDestination);
		if (!Destination)
		{
			if (Destination.error().Code != AssetOperationErrorCode::DestinationExists ||
				!AreCaseInsensitivePathComponentsEqual(SourcePath.parent_path(), DestinationPath.parent_path()) ||
				!AreCaseInsensitivePathComponentsEqual(SourcePath, DestinationPath) ||
				SourcePath.filename() == DestinationPath.filename())
			{
				return std::unexpected(Destination.error());
			}

			std::error_code EquivalentError;
			const bool SameAsset = std::filesystem::equivalent(*Source, AbsoluteDestination, EquivalentError);
			if (EquivalentError || !SameAsset)
				return std::unexpected(Destination.error());
			EquivalentError.clear();
			const bool SameSidecar = std::filesystem::equivalent(SourceSidecar, DestinationSidecar, EquivalentError);
			if (EquivalentError || !SameSidecar)
				return std::unexpected(Destination.error());

			const auto Metadata = LoadManagedMetadata(*Source, SourcePath);
			if (!Metadata)
				return std::unexpected(Metadata.error());
			if (Metadata->ID != SourceAsset)
			{
				return std::unexpected(MakeError(
					AssetOperationErrorCode::InvalidProjectAssets,
					SourcePath,
					"Asset sidecar identity changed while preparing the move; expected UUID " + SourceAsset.ToString()));
			}
			return RenameCaseOnlyAsset(
				Registry, *Project, SourceAsset, SourcePath, DestinationPath, *Source,
				AbsoluteDestination, SourceSidecar, DestinationSidecar);
		}
		const auto Metadata = LoadManagedMetadata(*Source, SourcePath);
		if (!Metadata)
			return std::unexpected(Metadata.error());
		if (Metadata->ID != SourceAsset)
		{
			return std::unexpected(MakeError(
				AssetOperationErrorCode::InvalidProjectAssets,
				SourcePath,
				"Asset sidecar identity changed while preparing the move; expected UUID " + SourceAsset.ToString()));
		}

		const FilesystemPath NormalizedDestinationSidecar = AssetMetadataSerializer::GetSidecarPath(*Destination);
		if (auto SidecarResult = RequireAbsent(NormalizedDestinationSidecar, DestinationPath); !SidecarResult)
			return std::unexpected(SidecarResult.error());

		std::error_code Error;
		if (!std::filesystem::copy_file(*Source, *Destination, std::filesystem::copy_options::none, Error) || Error)
		{
			const std::string CopyFailure = Error ? Error.message() : "destination already exists";
			if (Error == std::errc::file_exists)
			{
				return std::unexpected(MakeError(
					AssetOperationErrorCode::DestinationExists,
					DestinationPath,
					"Asset destination already exists"));
			}

			std::string CleanupFailure;
			if (!RemoveFileIfPresent(*Destination, CleanupFailure))
			{
				return std::unexpected(MakeError(
					AssetOperationErrorCode::RecoveryRequired,
					DestinationPath,
					"Asset copy failed (" + CopyFailure + ") and partial output could not be removed: " + CleanupFailure));
			}
			return std::unexpected(MakeError(
				AssetOperationErrorCode::FilesystemFailure,
				DestinationPath,
				"Could not create moved asset destination: " + CopyFailure));
		}

		Error.clear();
		if (!std::filesystem::copy_file(SourceSidecar, NormalizedDestinationSidecar, std::filesystem::copy_options::none, Error) || Error)
		{
			std::string RollbackFailure;
			const bool DestinationRemoved = RemoveFile(*Destination, RollbackFailure);
			std::string SidecarRollbackFailure;
			const bool PartialSidecarRemoved = Error == std::errc::file_exists ||
				RemoveFileIfPresent(NormalizedDestinationSidecar, SidecarRollbackFailure);
			if (!DestinationRemoved || !PartialSidecarRemoved)
			{
				return std::unexpected(MakeError(
					AssetOperationErrorCode::RecoveryRequired,
					DestinationPath,
					"Could not copy the sidecar and could not fully remove the partial destination. Source cleanup: " +
						RollbackFailure + "; sidecar cleanup: " + SidecarRollbackFailure));
			}
			return std::unexpected(MakeError(
				AssetOperationErrorCode::FilesystemFailure,
				DestinationPath,
				"Could not copy the asset sidecar: " + (Error ? Error.message() : "destination already exists")));
		}

		std::string RemoveFailure;
		if (!RemoveFile(*Source, RemoveFailure))
		{
			std::string RollbackFailure;
			if (!RemoveDestinationPair(*Destination, RollbackFailure))
			{
				return std::unexpected(MakeError(
					AssetOperationErrorCode::RecoveryRequired,
					DestinationPath,
					"Could not remove the source asset (" + RemoveFailure + ") and destination rollback failed: " + RollbackFailure));
			}
			return std::unexpected(MakeError(
				AssetOperationErrorCode::FilesystemFailure,
				SourcePath,
				"Could not remove the original asset after copying it: " + RemoveFailure));
		}

		if (!RemoveFile(SourceSidecar, RemoveFailure))
		{
			Error.clear();
			const bool RestoredSource = std::filesystem::copy_file(
				*Destination, *Source, std::filesystem::copy_options::none, Error);
			if (!RestoredSource || Error)
			{
				return std::unexpected(MakeError(
					AssetOperationErrorCode::RecoveryRequired,
					SourcePath,
					"Could not remove the source sidecar or restore the source asset. The destination contains the moved asset and "
						"the project registry will report the remaining orphaned sidecar. Destination: " +
						DestinationPath.generic_string()));
			}

			std::string RollbackFailure;
			if (!RemoveDestinationPair(*Destination, RollbackFailure))
			{
				return std::unexpected(MakeError(
					AssetOperationErrorCode::RecoveryRequired,
					DestinationPath,
					"Could not remove the source sidecar (" + RemoveFailure + ") and destination rollback failed: " + RollbackFailure));
			}
			return std::unexpected(MakeError(
				AssetOperationErrorCode::FilesystemFailure,
				SourcePath,
				"Could not remove the original sidecar; the source asset was restored: " + RemoveFailure));
		}

		if (auto RegistryResult = ValidateProjectRegistry(Registry, Project->Root, Refresh); !RegistryResult)
		{
			const AssetOperationError OriginalRegistryError = RegistryResult.error();
			const auto IsAbsent = [](const FilesystemPath& Path, std::error_code& StatusError)
			{
				const auto Status = std::filesystem::symlink_status(Path, StatusError);
				return StatusError == std::errc::no_such_file_or_directory ||
					(!StatusError && Status.type() == std::filesystem::file_type::not_found);
			};
			std::error_code SourceStatusError;
			std::error_code SidecarStatusError;
			const bool SourceAbsent = IsAbsent(*Source, SourceStatusError);
			const bool SidecarAbsent = IsAbsent(SourceSidecar, SidecarStatusError);
			std::string RollbackFailure;
			if ((SourceStatusError && SourceStatusError != std::errc::no_such_file_or_directory) ||
				(SidecarStatusError && SidecarStatusError != std::errc::no_such_file_or_directory) ||
				!SourceAbsent || !SidecarAbsent)
			{
				RollbackFailure = "the original asset path or sidecar path is occupied or cannot be inspected";
			}
			else
			{
				const auto DestinationMetadata = LoadManagedMetadata(*Destination, DestinationPath);
				if (!DestinationMetadata || DestinationMetadata->ID != SourceAsset)
				{
					RollbackFailure = DestinationMetadata
						? "the destination sidecar no longer has the moved asset UUID"
						: "the destination pair could not be verified: " + DestinationMetadata.error().Message;
				}
				else
				{
					Error.clear();
					std::filesystem::rename(*Destination, *Source, Error);
					if (Error)
						RollbackFailure = "could not restore the original asset file: " + Error.message();
					else
					{
						Error.clear();
						std::filesystem::rename(NormalizedDestinationSidecar, SourceSidecar, Error);
						if (Error)
						{
							const std::string SidecarFailure = Error.message();
							std::error_code RestoreMovedAssetError;
							std::filesystem::rename(*Source, *Destination, RestoreMovedAssetError);
							RollbackFailure = "could not restore the original sidecar: " + SidecarFailure;
							if (RestoreMovedAssetError)
								RollbackFailure += "; could not return the asset file to the destination: " +
									RestoreMovedAssetError.message();
							RollbackFailure += "; inspect source " + SourcePath.generic_string() +
								" and destination " + DestinationPath.generic_string();
						}
					}
				}
			}

			if (!RollbackFailure.empty())
			{
				AssetOperationError Recovery = MakeError(
					AssetOperationErrorCode::RecoveryRequired,
					DestinationPath,
					"The move could not be committed to a coherent registry after refresh failed (" +
						OriginalRegistryError.Message + "). Recovery is required: " + RollbackFailure +
						". Remaining paths: source=" + (Project->Root / SourcePath).generic_string() +
						", destination=" + (Project->Root / DestinationPath).generic_string() +
						", destination sidecar=" + AssetMetadataSerializer::GetSidecarPath(*Destination).generic_string());
				Recovery.RecoveryPath = *Destination;
				return std::unexpected(std::move(Recovery));
			}

			if (auto RollbackRefresh = ValidateProjectRegistry(Registry, Project->Root, Refresh); !RollbackRefresh)
			{
				AssetOperationError Recovery = MakeError(
					AssetOperationErrorCode::RecoveryRequired,
					SourcePath,
					"The asset files were restored to their original paths, but the registry could not confirm recovery: " +
						RollbackRefresh.error().Message);
				Recovery.RecoveryPath = *Source;
				return std::unexpected(std::move(Recovery));
			}
			return std::unexpected(MakeError(
				AssetOperationErrorCode::RegistryRefreshFailed,
				OriginalRegistryError.Path,
				"The asset move was rolled back because the post-move registry refresh failed: " +
					OriginalRegistryError.Message));
		}

		return AssetRecord{ Metadata->ID, DestinationPath };
	}

	std::expected<std::filesystem::path, AssetOperationError> AssetOperations::MoveFolder(
		AssetRegistry& Registry,
		const std::filesystem::path& ProjectRoot,
		const std::filesystem::path& SourceFolderPath,
		const std::filesystem::path& DestinationParentPath)
	{
		const auto Project = OpenProject(ProjectRoot);
		if (!Project)
			return std::unexpected(Project.error());
		if (auto RegistryResult = ValidateProjectRegistry(Registry, Project->Root); !RegistryResult)
			return std::unexpected(RegistryResult.error());
		if (IsReparsePoint(Project->Root))
		{
			return std::unexpected(MakeError(
				AssetOperationErrorCode::InvalidProject,
				Project->Root,
				"Folder moves require a project root that is not a Windows reparse point"));
		}
		if (auto SourceResult = ValidateManagedDirectory(*Project, SourceFolderPath, false); !SourceResult)
			return std::unexpected(SourceResult.error());
		if (auto DestinationResult = ValidateManagedDirectory(*Project, DestinationParentPath, true); !DestinationResult)
			return std::unexpected(DestinationResult.error());

		if (IsCaseInsensitivePathPrefix(SourceFolderPath, DestinationParentPath))
		{
			return std::unexpected(MakeError(
				AssetOperationErrorCode::InvalidPath,
				DestinationParentPath,
				"A managed folder cannot be moved into itself or one of its descendants"));
		}

		const FilesystemPath DestinationPath = DestinationParentPath / SourceFolderPath.filename();
		if (DestinationPath == SourceFolderPath)
		{
			return std::unexpected(MakeError(
				AssetOperationErrorCode::InvalidPath,
				SourceFolderPath,
				"Folder source and destination parent must differ"));
		}

		const FilesystemPath AbsoluteSource = Project->Root / SourceFolderPath;
		const FilesystemPath AbsoluteDestination = Project->Root / DestinationPath;
		std::error_code Error;
		const auto DestinationStatus = std::filesystem::symlink_status(AbsoluteDestination, Error);
		const bool DestinationExists = !Error && DestinationStatus.type() != std::filesystem::file_type::not_found;
		if (Error && Error != std::errc::no_such_file_or_directory)
		{
			return std::unexpected(MakeError(
				AssetOperationErrorCode::FilesystemFailure,
				DestinationPath,
				"Could not inspect the destination folder: " + Error.message()));
		}
		if (DestinationExists)
		{
			return std::unexpected(MakeError(
				AssetOperationErrorCode::DestinationExists,
				DestinationPath,
				"A folder with the same name already exists in the destination"));
		}

		if (auto TreeResult = ValidateNoReparseEntries(AbsoluteSource, SourceFolderPath); !TreeResult)
			return std::unexpected(TreeResult.error());

		// A sibling staging name makes the filesystem rename transactional even for case-only paths
		// on Windows. The source and destination are both below one non-reparse Assets tree.
		const auto TransactionID = UUID::Generate();
		if (!TransactionID)
		{
			return std::unexpected(MakeError(
				AssetOperationErrorCode::FilesystemFailure,
				SourceFolderPath,
				"Could not create a unique temporary name for the folder move"));
		}
		const FilesystemPath StagingPath = AbsoluteSource.parent_path() /
			(AbsoluteSource.filename().native() + std::filesystem::path(L".pulseforge-move-").native() +
				std::filesystem::path(TransactionID->ToString()).native());
		if (std::filesystem::exists(StagingPath, Error) || Error)
		{
			return std::unexpected(MakeError(
				AssetOperationErrorCode::FilesystemFailure,
				SourceFolderPath,
				"Could not reserve a unique temporary folder-move path"));
		}

		std::filesystem::rename(AbsoluteSource, StagingPath, Error);
		if (Error)
		{
			return std::unexpected(MakeError(
				AssetOperationErrorCode::FilesystemFailure,
				SourceFolderPath,
				"Could not stage the source folder for moving: " + Error.message()));
		}

		Error.clear();
		std::filesystem::rename(StagingPath, AbsoluteDestination, Error);
		if (Error)
		{
			const std::string MoveFailure = Error.message();
			Error.clear();
			std::filesystem::rename(StagingPath, AbsoluteSource, Error);
			if (Error)
			{
				return std::unexpected(MakeError(
					AssetOperationErrorCode::RecoveryRequired,
					SourceFolderPath,
					"Folder move failed and the staged source could not be restored. Recovery directory: " +
						StagingPath.generic_string() + "; restore failure: " + Error.message()));
			}
			return std::unexpected(MakeError(
				AssetOperationErrorCode::FilesystemFailure,
				DestinationPath,
				"Could not move the staged folder into the destination: " + MoveFailure));
		}

		if (auto RegistryResult = ValidateProjectRegistry(Registry, Project->Root); !RegistryResult)
		{
			const std::string RefreshFailure = RegistryResult.error().Message;
			Error.clear();
			std::filesystem::rename(AbsoluteDestination, AbsoluteSource, Error);
			if (Error)
			{
				return std::unexpected(MakeError(
					AssetOperationErrorCode::RecoveryRequired,
					DestinationPath,
					"Folder was moved but the registry refresh failed, and the move could not be rolled back: " +
						RefreshFailure + "; rollback failure: " + Error.message()));
			}
			if (auto RollbackRefresh = ValidateProjectRegistry(Registry, Project->Root); !RollbackRefresh)
			{
				return std::unexpected(MakeError(
					AssetOperationErrorCode::RecoveryRequired,
					SourceFolderPath,
					"Folder move was rolled back, but the original asset registry could not be rebuilt: " +
						RollbackRefresh.error().Message));
			}
			return std::unexpected(MakeError(
				AssetOperationErrorCode::RegistryRefreshFailed,
				DestinationPath,
				"Folder move was rolled back because the asset registry refresh failed: " + RefreshFailure));
		}

		return DestinationPath;
	}

	std::expected<AssetRecord, AssetOperationError> AssetOperations::Duplicate(
		AssetRegistry& Registry,
		const std::filesystem::path& ProjectRoot,
		const AssetID& SourceAsset,
		const std::filesystem::path& DestinationPath)
	{
		const auto Project = OpenProject(ProjectRoot);
		if (!Project)
			return std::unexpected(Project.error());
		if (auto RegistryResult = ValidateProjectRegistry(Registry, Project->Root); !RegistryResult)
			return std::unexpected(RegistryResult.error());
		const auto SourceRecord = Registry.Find(SourceAsset);
		if (!SourceRecord)
		{
			return std::unexpected(MakeError(
				AssetOperationErrorCode::AssetNotFound,
				{},
				"Asset UUID " + SourceAsset.ToString() + " is not present in the project registry"));
		}
		const FilesystemPath SourcePath = SourceRecord->ProjectRelativePath;
		if (SourcePath == DestinationPath)
		{
			return std::unexpected(MakeError(
				AssetOperationErrorCode::InvalidPath,
				SourcePath,
				"Asset source and destination paths must differ"));
		}

		const auto Source = ResolveAssetPath(*Project, SourcePath, true);
		if (!Source)
			return std::unexpected(Source.error());
		const auto Destination = ResolveAssetPath(*Project, DestinationPath, false);
		if (!Destination)
			return std::unexpected(Destination.error());
		const auto SourceMetadata = LoadManagedMetadata(*Source, SourcePath);
		if (!SourceMetadata)
			return std::unexpected(SourceMetadata.error());
		if (SourceMetadata->ID != SourceAsset)
		{
			return std::unexpected(MakeError(
				AssetOperationErrorCode::InvalidProjectAssets,
				SourcePath,
				"Asset sidecar identity changed while preparing the duplicate; expected UUID " + SourceAsset.ToString()));
		}
		if (auto SidecarResult = RequireAbsent(AssetMetadataSerializer::GetSidecarPath(*Destination), DestinationPath);
			!SidecarResult)
		{
			return std::unexpected(SidecarResult.error());
		}

		std::error_code Error;
		if (!std::filesystem::copy_file(*Source, *Destination, std::filesystem::copy_options::none, Error) || Error)
		{
			const std::string CopyFailure = Error ? Error.message() : "destination already exists";
			if (Error == std::errc::file_exists)
			{
				return std::unexpected(MakeError(
					AssetOperationErrorCode::DestinationExists,
					DestinationPath,
					"Asset destination already exists"));
			}

			std::string CleanupFailure;
			if (!RemoveFileIfPresent(*Destination, CleanupFailure))
			{
				return std::unexpected(MakeError(
					AssetOperationErrorCode::RecoveryRequired,
					DestinationPath,
					"Asset copy failed (" + CopyFailure + ") and partial output could not be removed: " + CleanupFailure));
			}
			return std::unexpected(MakeError(
				AssetOperationErrorCode::FilesystemFailure,
				DestinationPath,
				"Could not copy asset source: " + CopyFailure));
		}

		const auto NewMetadata = AssetMetadataSerializer::CreateDuplicateForNewAsset(*Source, *Destination);
		if (!NewMetadata)
		{
			std::string RollbackFailure;
			const bool DestinationRemoved = RemoveFile(*Destination, RollbackFailure);
			std::string SidecarRollbackFailure;
			const bool SidecarRemoved = NewMetadata.error().Code == AssetMetadataErrorCode::MetadataAlreadyExists ||
				RemoveFileIfPresent(AssetMetadataSerializer::GetSidecarPath(*Destination), SidecarRollbackFailure);
			if (!DestinationRemoved || !SidecarRemoved)
			{
				return std::unexpected(MakeError(
					AssetOperationErrorCode::RecoveryRequired,
					DestinationPath,
					"Could not create duplicate metadata (" + NewMetadata.error().Message +
					") and could not fully remove the copied source. Source cleanup: " + RollbackFailure +
						"; sidecar cleanup: " + SidecarRollbackFailure));
			}
			return std::unexpected(MakeMetadataOperationError(NewMetadata.error(), DestinationPath));
		}

		if (auto RegistryResult = ValidateProjectRegistry(Registry, Project->Root); !RegistryResult)
		{
			std::string RollbackFailure;
			if (!RemoveDestinationPair(*Destination, RollbackFailure))
			{
				return std::unexpected(MakeError(
					AssetOperationErrorCode::RecoveryRequired,
					DestinationPath,
					"The copied asset failed registry validation (" + RegistryResult.error().Message +
					") and rollback failed: " + RollbackFailure));
			}
			if (auto RefreshResult = ValidateProjectRegistry(Registry, Project->Root); !RefreshResult)
				return std::unexpected(MakeRegistryRefreshError(RefreshResult.error(), "Duplicate rollback"));
			return std::unexpected(RegistryResult.error());
		}

		return AssetRecord{ NewMetadata->ID, DestinationPath };
	}

	std::expected<void, AssetOperationError> AssetOperations::Delete(
		AssetRegistry& Registry,
		const std::filesystem::path& ProjectRoot,
		const AssetID& SourceAsset)
	{
		const auto Project = OpenProject(ProjectRoot);
		if (!Project)
			return std::unexpected(Project.error());
		if (auto RegistryResult = ValidateProjectRegistry(Registry, Project->Root); !RegistryResult)
			return std::unexpected(RegistryResult.error());
		const auto SourceRecord = Registry.Find(SourceAsset);
		if (!SourceRecord)
		{
			return std::unexpected(MakeError(
				AssetOperationErrorCode::AssetNotFound,
				{},
				"Asset UUID " + SourceAsset.ToString() + " is not present in the project registry"));
		}
		const FilesystemPath SourcePath = SourceRecord->ProjectRelativePath;

		const auto Source = ResolveAssetPath(*Project, SourcePath, true);
		if (!Source)
			return std::unexpected(Source.error());
		const auto Metadata = LoadManagedMetadata(*Source, SourcePath);
		if (!Metadata)
			return std::unexpected(Metadata.error());
		if (Metadata->ID != SourceAsset)
		{
			return std::unexpected(MakeError(
				AssetOperationErrorCode::InvalidProjectAssets,
				SourcePath,
				"Asset sidecar identity changed while preparing deletion; expected UUID " + SourceAsset.ToString()));
		}
		const FilesystemPath Sidecar = AssetMetadataSerializer::GetSidecarPath(*Source);

		const auto TransactionID = UUID::Generate();
		if (!TransactionID)
		{
			return std::unexpected(MakeError(
				AssetOperationErrorCode::FilesystemFailure,
				SourcePath,
				"Could not create a temporary deletion identity: " + TransactionID.error().Message));
		}

		FilesystemPath StagingRoot = Project->Root;
		for (const char* DirectoryName : { ".pulseforge", "cache", "asset-operations" })
		{
			StagingRoot /= DirectoryName;
			std::error_code Error;
			const auto Status = std::filesystem::symlink_status(StagingRoot, Error);
			const bool Missing = Error == std::errc::no_such_file_or_directory ||
				(!Error && Status.type() == std::filesystem::file_type::not_found);
			if (Error && !Missing)
			{
				return std::unexpected(MakeError(
					AssetOperationErrorCode::FilesystemFailure,
					StagingRoot.lexically_relative(Project->Root),
					"Could not inspect asset operation staging directory: " + Error.message()));
			}
			if (!Missing && (std::filesystem::is_symlink(Status) || !std::filesystem::is_directory(Status)))
			{
				return std::unexpected(MakeError(
					AssetOperationErrorCode::InvalidProject,
					StagingRoot.lexically_relative(Project->Root),
					"Asset operation staging path must be a regular, non-symlink directory"));
			}
			if (Missing)
			{
				Error.clear();
				if (!std::filesystem::create_directory(StagingRoot, Error) || Error)
				{
					return std::unexpected(MakeError(
						AssetOperationErrorCode::FilesystemFailure,
						StagingRoot.lexically_relative(Project->Root),
						"Could not create asset operation staging directory: " + (Error ? Error.message() : "directory was not created")));
				}
			}
		}

		const FilesystemPath TransactionDirectory = StagingRoot / TransactionID->ToString();
		std::error_code Error;
		if (!std::filesystem::create_directory(TransactionDirectory, Error) || Error)
		{
			return std::unexpected(MakeError(
				AssetOperationErrorCode::FilesystemFailure,
				TransactionDirectory.lexically_relative(Project->Root),
				"Could not create asset deletion staging directory: " + (Error ? Error.message() : "directory already exists")));
		}

		const FilesystemPath StagedSource = TransactionDirectory / Source->filename();
		const FilesystemPath StagedSidecar = AssetMetadataSerializer::GetSidecarPath(StagedSource);
		if (!std::filesystem::copy_file(*Source, StagedSource, std::filesystem::copy_options::none, Error) || Error)
		{
			const std::string StageFailure = Error ? Error.message() : "staging copy failed";
			std::string CleanupFailure;
			if (!RemoveTransactionDirectory(TransactionDirectory, CleanupFailure))
			{
				return std::unexpected(MakeError(
					AssetOperationErrorCode::RecoveryRequired,
					TransactionDirectory.lexically_relative(Project->Root),
					"Could not stage the managed asset (" + StageFailure + ") or clean the partial staging data: " +
						CleanupFailure));
			}
			return std::unexpected(MakeError(
				AssetOperationErrorCode::FilesystemFailure,
				SourcePath,
				"Could not stage managed asset before deletion: " + StageFailure));
		}
		Error.clear();
		if (!std::filesystem::copy_file(Sidecar, StagedSidecar, std::filesystem::copy_options::none, Error) || Error)
		{
			const std::string StageFailure = Error ? Error.message() : "staging copy failed";
			std::string CleanupFailure;
			if (!RemoveTransactionDirectory(TransactionDirectory, CleanupFailure))
			{
				return std::unexpected(MakeError(
					AssetOperationErrorCode::RecoveryRequired,
					TransactionDirectory.lexically_relative(Project->Root),
					"Could not stage the sidecar (" + StageFailure + ") or clean the partial staging data: " + CleanupFailure));
			}
			return std::unexpected(MakeError(
				AssetOperationErrorCode::FilesystemFailure,
				SourcePath,
				"Could not stage managed sidecar before deletion: " + StageFailure));
		}

		std::string RemoveFailure;
		if (!RemoveFile(*Source, RemoveFailure))
		{
			std::string CleanupFailure;
			if (!RemoveTransactionDirectory(TransactionDirectory, CleanupFailure))
			{
				return std::unexpected(MakeError(
					AssetOperationErrorCode::RecoveryRequired,
					TransactionDirectory.lexically_relative(Project->Root),
					"Could not remove the managed asset (" + RemoveFailure + ") or its staging data: " + CleanupFailure));
			}
			return std::unexpected(MakeError(
				AssetOperationErrorCode::FilesystemFailure,
				SourcePath,
				"Could not remove managed asset: " + RemoveFailure));
		}
		if (!RemoveFile(Sidecar, RemoveFailure))
		{
			Error.clear();
			const bool Restored = std::filesystem::copy_file(
				StagedSource, *Source, std::filesystem::copy_options::none, Error);
			if (!Restored || Error)
			{
				return std::unexpected(MakeError(
					AssetOperationErrorCode::RecoveryRequired,
					TransactionDirectory.lexically_relative(Project->Root),
					"Could not remove the sidecar or restore the source asset. Recovery copy remains at: " +
						TransactionDirectory.generic_string()));
			}
			std::string CleanupFailure;
			if (!RemoveTransactionDirectory(TransactionDirectory, CleanupFailure))
			{
				return std::unexpected(MakeError(
					AssetOperationErrorCode::RecoveryRequired,
					TransactionDirectory.lexically_relative(Project->Root),
					"The asset source was restored, but its temporary recovery copy remains: " + CleanupFailure));
			}
			return std::unexpected(MakeError(
				AssetOperationErrorCode::FilesystemFailure,
				SourcePath,
				"Could not remove managed sidecar; the asset source was restored: " + RemoveFailure));
		}

		Error.clear();
		std::filesystem::remove_all(TransactionDirectory, Error);
		if (Error)
		{
			std::string Message = "Managed asset was removed, but its temporary recovery copy could not be deleted: " +
				Error.message();
			if (auto RegistryResult = ValidateProjectRegistry(Registry, Project->Root); !RegistryResult)
				Message += "\nThe supplied registry could not be refreshed after deletion: " + RegistryResult.error().Message;

			return std::unexpected(MakeError(
				AssetOperationErrorCode::RecoveryRequired,
				TransactionDirectory.lexically_relative(Project->Root),
				std::move(Message)));
		}

		if (auto RegistryResult = ValidateProjectRegistry(Registry, Project->Root); !RegistryResult)
			return std::unexpected(MakeRegistryRefreshError(RegistryResult.error(), "Asset deletion"));

		return {};
	}
}
