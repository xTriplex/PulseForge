#include "Core/PulseForgePCH.h"
#include "Assets/AssetOperations.h"

#include "Assets/AssetMetadata.h"

#include <iterator>
#include <system_error>

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

		std::expected<void, AssetOperationError> ValidateProjectRegistry(const FilesystemPath& ProjectRoot)
		{
			AssetRegistry Registry;
			const auto RebuildResult = Registry.Rebuild(ProjectRoot);
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

	std::expected<AssetRecord, AssetOperationError> AssetOperations::Move(
		const std::filesystem::path& ProjectRoot,
		const std::filesystem::path& SourcePath,
		const std::filesystem::path& DestinationPath)
	{
		const auto Project = OpenProject(ProjectRoot);
		if (!Project)
			return std::unexpected(Project.error());
		if (auto RegistryResult = ValidateProjectRegistry(Project->Root); !RegistryResult)
			return std::unexpected(RegistryResult.error());
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
		const auto Metadata = LoadManagedMetadata(*Source, SourcePath);
		if (!Metadata)
			return std::unexpected(Metadata.error());

		const FilesystemPath SourceSidecar = AssetMetadataSerializer::GetSidecarPath(*Source);
		const FilesystemPath DestinationSidecar = AssetMetadataSerializer::GetSidecarPath(*Destination);
		if (auto SidecarResult = RequireAbsent(DestinationSidecar, DestinationPath); !SidecarResult)
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
		if (!std::filesystem::copy_file(SourceSidecar, DestinationSidecar, std::filesystem::copy_options::none, Error) || Error)
		{
			std::string RollbackFailure;
			const bool DestinationRemoved = RemoveFile(*Destination, RollbackFailure);
			std::string SidecarRollbackFailure;
			const bool PartialSidecarRemoved = Error == std::errc::file_exists ||
				RemoveFileIfPresent(DestinationSidecar, SidecarRollbackFailure);
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

		return AssetRecord{ Metadata->ID, DestinationPath };
	}

	std::expected<AssetRecord, AssetOperationError> AssetOperations::Duplicate(
		const std::filesystem::path& ProjectRoot,
		const std::filesystem::path& SourcePath,
		const std::filesystem::path& DestinationPath)
	{
		const auto Project = OpenProject(ProjectRoot);
		if (!Project)
			return std::unexpected(Project.error());
		if (auto RegistryResult = ValidateProjectRegistry(Project->Root); !RegistryResult)
			return std::unexpected(RegistryResult.error());
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

		if (auto RegistryResult = ValidateProjectRegistry(Project->Root); !RegistryResult)
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
			return std::unexpected(RegistryResult.error());
		}

		return AssetRecord{ NewMetadata->ID, DestinationPath };
	}

	std::expected<void, AssetOperationError> AssetOperations::Delete(
		const std::filesystem::path& ProjectRoot,
		const std::filesystem::path& SourcePath)
	{
		const auto Project = OpenProject(ProjectRoot);
		if (!Project)
			return std::unexpected(Project.error());
		if (auto RegistryResult = ValidateProjectRegistry(Project->Root); !RegistryResult)
			return std::unexpected(RegistryResult.error());

		const auto Source = ResolveAssetPath(*Project, SourcePath, true);
		if (!Source)
			return std::unexpected(Source.error());
		const auto Metadata = LoadManagedMetadata(*Source, SourcePath);
		if (!Metadata)
			return std::unexpected(Metadata.error());
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
			return std::unexpected(MakeError(
				AssetOperationErrorCode::RecoveryRequired,
				TransactionDirectory.lexically_relative(Project->Root),
				"Managed asset was removed, but its temporary recovery copy could not be deleted: " + Error.message()));
		}

		return {};
	}
}
