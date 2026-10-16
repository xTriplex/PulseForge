#include "TestAssetSupport.h"

namespace
{
	void TestAssetOperations(TestRunner& Tests)
	{
		using namespace PulseForge;
		std::error_code FileError;
		const std::filesystem::path TemporaryDirectory = std::filesystem::temp_directory_path(FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError) return;

		const auto ProjectIdentifier = UUID::Generate();
		PF_CHECK(Tests, ProjectIdentifier.has_value());
		if (!ProjectIdentifier) return;

		const std::filesystem::path ProjectRoot = TemporaryDirectory / ("PulseForgeAssetOperations-" + ProjectIdentifier->ToString());
		struct ProjectDirectoryCleanup
		{
			std::filesystem::path Path;
			~ProjectDirectoryCleanup()
			{
				std::error_code Error;
				std::filesystem::remove_all(Path, Error);
			}
		} Cleanup{ ProjectRoot };

		const std::filesystem::path AssetDirectory = ProjectRoot / "Assets" / "Models";
		std::filesystem::create_directories(AssetDirectory, FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError) return;

		const auto WriteFile = [](const std::filesystem::path& Path, std::string_view Data)
		{
			std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
			if (!Output) return false;
			Output.write(Data.data(), static_cast<std::streamsize>(Data.size()));
			Output.close();
			return static_cast<bool>(Output);
		};
		const auto ReadFile = [](const std::filesystem::path& Path) -> std::optional<std::string>
		{
			std::ifstream Input(Path, std::ios::binary);
			if (!Input) return std::nullopt;
			std::string Data{ std::istreambuf_iterator<char>(Input), std::istreambuf_iterator<char>() };
			if (Input.bad()) return std::nullopt;
			return Data;
		};

		const std::filesystem::path Source = AssetDirectory / "ship.gltf";
		PF_CHECK(Tests, WriteFile(Source, "managed model source"));
		const auto SourceMetadata = AssetMetadataSerializer::CreateForNewAsset(Source);
		PF_CHECK(Tests, SourceMetadata.has_value());
		if (!SourceMetadata) return;
		const std::filesystem::path SourceSidecar = AssetMetadataSerializer::GetSidecarPath(Source);
		const std::string MetadataWithImportSettings = "{\"format\":\"PulseForgeAssetMeta\",\"version\":1,\"uuid\":\"" +
													   SourceMetadata->ID.ToString() + "\",\"importSettings\":{\"scale\":2.5}}";
		PF_CHECK(Tests, WriteFile(SourceSidecar, MetadataWithImportSettings));

		AssetRegistry InitialRegistry;
		PF_CHECK(Tests, InitialRegistry.Rebuild(ProjectRoot).has_value());
		PF_CHECK(Tests, InitialRegistry.GetAssetCount() == 1);

		const auto UnknownAsset = UUID::Generate();
		PF_CHECK(Tests, UnknownAsset.has_value());
		if (!UnknownAsset) return;
		const auto UnknownMove = AssetOperations::Move(InitialRegistry, ProjectRoot, *UnknownAsset, "Assets/Models/not-found-move.gltf");
		PF_CHECK(Tests, !UnknownMove && UnknownMove.error().Code == AssetOperationErrorCode::AssetNotFound);
		PF_CHECK(Tests, !UnknownMove && UnknownMove.error().Message.find(UnknownAsset->ToString()) != std::string::npos);
		const auto UnknownDuplicate =
			AssetOperations::Duplicate(InitialRegistry, ProjectRoot, *UnknownAsset, "Assets/Models/not-found-duplicate.gltf");
		PF_CHECK(Tests, !UnknownDuplicate && UnknownDuplicate.error().Code == AssetOperationErrorCode::AssetNotFound);
		const auto UnknownDelete = AssetOperations::Delete(InitialRegistry, ProjectRoot, *UnknownAsset);
		PF_CHECK(Tests, !UnknownDelete && UnknownDelete.error().Code == AssetOperationErrorCode::AssetNotFound);
		PF_CHECK(Tests, InitialRegistry.GetAssetCount() == 1);
		PF_CHECK(Tests, std::filesystem::exists(Source) && std::filesystem::exists(SourceSidecar));
		PF_CHECK(Tests, !std::filesystem::exists(ProjectRoot / "Assets/Models/not-found-move.gltf"));
		PF_CHECK(Tests, !std::filesystem::exists(ProjectRoot / "Assets/Models/not-found-duplicate.gltf"));

		const std::filesystem::path MovedPath = "Assets/Models/ship-renamed.gltf";
		const auto MoveResult = AssetOperations::Move(InitialRegistry, ProjectRoot, SourceMetadata->ID, MovedPath);
		PF_CHECK(Tests, MoveResult.has_value());
		PF_CHECK(Tests, MoveResult && MoveResult->ID == SourceMetadata->ID);
		PF_CHECK(Tests, MoveResult && MoveResult->ProjectRelativePath == MovedPath);
		const std::filesystem::path MovedSource = ProjectRoot / MovedPath;
		PF_CHECK(Tests, !std::filesystem::exists(Source));
		PF_CHECK(Tests, !std::filesystem::exists(SourceSidecar));
		PF_CHECK(Tests, ReadFile(MovedSource) == std::optional<std::string>("managed model source"));
		const std::filesystem::path MovedSidecarPath = AssetMetadataSerializer::GetSidecarPath(MovedSource);
		PF_CHECK(Tests, ReadFile(MovedSidecarPath) == std::optional<std::string>(MetadataWithImportSettings));
		const auto MovedMetadata = AssetMetadataSerializer::LoadFromFile(MovedSidecarPath);
		PF_CHECK(Tests, MovedMetadata && MovedMetadata->ID == SourceMetadata->ID);

		const auto MovedRecord = InitialRegistry.Find(SourceMetadata->ID);
		PF_CHECK(Tests, MovedRecord && MovedRecord->ProjectRelativePath == MovedPath);

#if defined(PF_DEBUG)
		{
			const std::filesystem::path RollbackPath = "Assets/Models/registry-rollback.gltf";
			uint32_t RefreshCount = 0;
			const AssetOperations::RegistryRefresh FailPostMoveRefresh = [&](const auto& Root) -> std::expected<void, AssetRegistryError>
			{
				if (++RefreshCount == 2)
				{
					AssetRegistryError Failure;
					Failure.Issues.push_back(
						{ AssetRegistryIssueCode::AssetScanFailed, "Assets", {}, "Injected final registry refresh failure" });
					return std::unexpected(std::move(Failure));
				}
				return InitialRegistry.Rebuild(Root);
			};
			const auto RolledBackMove = AssetOperations::MoveWithRegistryRefreshForTesting(
				InitialRegistry, ProjectRoot, SourceMetadata->ID, RollbackPath, FailPostMoveRefresh);
			const auto RolledBackRecord = InitialRegistry.Find(SourceMetadata->ID);
			PF_CHECK(Tests, !RolledBackMove && RolledBackMove.error().Code == AssetOperationErrorCode::RegistryRefreshFailed);
			PF_CHECK(
				Tests, RefreshCount == 3 && std::filesystem::exists(MovedSource) && !std::filesystem::exists(ProjectRoot / RollbackPath));
			PF_CHECK(Tests, RolledBackRecord && RolledBackRecord->ProjectRelativePath == MovedPath);
			PF_CHECK(Tests, ReadFile(MovedSidecarPath) == std::optional<std::string>(MetadataWithImportSettings));

			const std::filesystem::path RecoveryPath = "Assets/Models/registry-recovery.gltf";
			bool ConflictCreated = false;
			RefreshCount = 0;
			const AssetOperations::RegistryRefresh FailRollbackRefresh = [&](const auto& Root) -> std::expected<void, AssetRegistryError>
			{
				if (++RefreshCount == 2)
				{
					ConflictCreated = WriteFile(MovedSource, "concurrent source path");
					AssetRegistryError Failure;
					Failure.Issues.push_back(
						{ AssetRegistryIssueCode::AssetScanFailed, "Assets", {}, "Injected final registry refresh failure" });
					return std::unexpected(std::move(Failure));
				}
				return InitialRegistry.Rebuild(Root);
			};
			const auto RecoveryMove = AssetOperations::MoveWithRegistryRefreshForTesting(
				InitialRegistry, ProjectRoot, SourceMetadata->ID, RecoveryPath, FailRollbackRefresh);
			const auto RecoveryAsset = ProjectRoot / RecoveryPath;
			PF_CHECK(Tests, !RecoveryMove && RecoveryMove.error().Code == AssetOperationErrorCode::RecoveryRequired);
			PF_CHECK(Tests, ConflictCreated && ReadFile(MovedSource) == std::optional<std::string>("concurrent source path"));
			PF_CHECK(Tests,
				std::filesystem::exists(RecoveryAsset) && std::filesystem::exists(AssetMetadataSerializer::GetSidecarPath(RecoveryAsset)));
			PF_CHECK(Tests, !RecoveryMove && RecoveryMove.error().RecoveryPath == RecoveryAsset);
			std::filesystem::remove(MovedSource, FileError);
			PF_CHECK(Tests, !FileError);
			std::filesystem::rename(RecoveryAsset, MovedSource, FileError);
			PF_CHECK(Tests, !FileError);
			std::filesystem::rename(AssetMetadataSerializer::GetSidecarPath(RecoveryAsset), MovedSidecarPath, FileError);
			PF_CHECK(Tests, !FileError);
			PF_CHECK(Tests, InitialRegistry.Rebuild(ProjectRoot).has_value());
			const auto RecoveredRecord = InitialRegistry.Find(SourceMetadata->ID);
			PF_CHECK(Tests, RecoveredRecord && RecoveredRecord->ProjectRelativePath == MovedPath);
		}
#endif

		const std::filesystem::path DuplicatePath = "Assets/Models/ship-copy.gltf";
		const auto DuplicateResult = AssetOperations::Duplicate(InitialRegistry, ProjectRoot, SourceMetadata->ID, DuplicatePath);
		PF_CHECK(Tests, DuplicateResult.has_value());
		PF_CHECK(Tests, DuplicateResult && DuplicateResult->ID != SourceMetadata->ID);
		PF_CHECK(Tests, DuplicateResult && DuplicateResult->ProjectRelativePath == DuplicatePath);
		PF_CHECK(Tests, InitialRegistry.GetAssetCount() == 2);
		const std::filesystem::path DuplicateSource = ProjectRoot / DuplicatePath;
		PF_CHECK(Tests, ReadFile(DuplicateSource) == std::optional<std::string>("managed model source"));
		const auto DuplicateSidecar = ReadFile(AssetMetadataSerializer::GetSidecarPath(DuplicateSource));
		PF_CHECK(Tests, DuplicateSidecar && DuplicateSidecar->find("\"scale\": 2.5") != std::string::npos);
		const auto DuplicateMetadata = AssetMetadataSerializer::LoadFromFile(AssetMetadataSerializer::GetSidecarPath(DuplicateSource));
		PF_CHECK(Tests, DuplicateMetadata && DuplicateMetadata->ID == DuplicateResult->ID);

		const auto ExistingDestination = AssetOperations::Duplicate(InitialRegistry, ProjectRoot, SourceMetadata->ID, DuplicatePath);
		PF_CHECK(Tests, !ExistingDestination.has_value());
		PF_CHECK(Tests, !ExistingDestination && ExistingDestination.error().Code == AssetOperationErrorCode::DestinationExists);
		const auto ExistingMoveDestination = AssetOperations::Move(InitialRegistry, ProjectRoot, SourceMetadata->ID, DuplicatePath);
		PF_CHECK(Tests, !ExistingMoveDestination.has_value());
		PF_CHECK(Tests, !ExistingMoveDestination && ExistingMoveDestination.error().Code == AssetOperationErrorCode::DestinationExists);
		PF_CHECK(Tests, std::filesystem::exists(MovedSource));
		PF_CHECK(Tests, std::filesystem::exists(DuplicateSource));
		const auto InvalidTraversal = AssetOperations::Move(InitialRegistry, ProjectRoot, SourceMetadata->ID, "Assets/../escaped.gltf");
		PF_CHECK(Tests, !InvalidTraversal.has_value());
		PF_CHECK(Tests, !InvalidTraversal && InvalidTraversal.error().Code == AssetOperationErrorCode::InvalidPath);

		const std::filesystem::path CaseChangedPath = "Assets/Models/SHIP-RENAMED.gltf";
		const auto CaseChangedMove = AssetOperations::Move(InitialRegistry, ProjectRoot, SourceMetadata->ID, CaseChangedPath);
		PF_CHECK(Tests, CaseChangedMove && CaseChangedMove->ID == SourceMetadata->ID);
		PF_CHECK(Tests, CaseChangedMove && CaseChangedMove->ProjectRelativePath == CaseChangedPath);
		const std::filesystem::path CaseChangedSource = ProjectRoot / CaseChangedPath;
		PF_CHECK(Tests, std::filesystem::exists(CaseChangedSource));
		PF_CHECK(Tests, ReadFile(CaseChangedSource) == std::optional<std::string>("managed model source"));
		PF_CHECK(Tests,
			ReadFile(AssetMetadataSerializer::GetSidecarPath(CaseChangedSource)) == std::optional<std::string>(MetadataWithImportSettings));
		const auto CaseChangedRecord = InitialRegistry.Find(SourceMetadata->ID);
		PF_CHECK(Tests, CaseChangedRecord && CaseChangedRecord->ProjectRelativePath == CaseChangedPath);

		const auto CaseRestoredMove = AssetOperations::Move(InitialRegistry, ProjectRoot, SourceMetadata->ID, MovedPath);
		PF_CHECK(Tests, CaseRestoredMove && CaseRestoredMove->ID == SourceMetadata->ID);
		PF_CHECK(Tests, CaseRestoredMove && CaseRestoredMove->ProjectRelativePath == MovedPath);
		PF_CHECK(Tests, ReadFile(MovedSource) == std::optional<std::string>("managed model source"));
		PF_CHECK(Tests, ReadFile(MovedSidecarPath) == std::optional<std::string>(MetadataWithImportSettings));

		PF_CHECK(Tests, DuplicateMetadata && AssetOperations::Delete(InitialRegistry, ProjectRoot, DuplicateMetadata->ID).has_value());
		PF_CHECK(Tests, !std::filesystem::exists(DuplicateSource));
		PF_CHECK(Tests, !std::filesystem::exists(AssetMetadataSerializer::GetSidecarPath(DuplicateSource)));
		PF_CHECK(Tests, InitialRegistry.GetAssetCount() == 1);
		PF_CHECK(Tests, !InitialRegistry.Find(DuplicateMetadata->ID).has_value());
		const bool TransactionDirectoryEmpty =
			std::filesystem::is_empty(ProjectRoot / ".pulseforge" / "cache" / "asset-operations", FileError);
		PF_CHECK(Tests, !FileError && TransactionDirectoryEmpty);

		const std::filesystem::path AliasedSource = AssetDirectory / "aliased.gltf";
		PF_CHECK(Tests, WriteFile(AliasedSource, "duplicate identity source"));
		PF_CHECK(Tests, WriteFile(AssetMetadataSerializer::GetSidecarPath(AliasedSource),
							"{\"format\":\"PulseForgeAssetMeta\",\"version\":1,\"uuid\":\"" + SourceMetadata->ID.ToString() +
								"\",\"importSettings\":{}}"));
		const std::filesystem::path CollisionCopyPath = "Assets/Models/collision-copy.gltf";
		const auto DuplicateInInvalidProject =
			AssetOperations::Duplicate(InitialRegistry, ProjectRoot, SourceMetadata->ID, CollisionCopyPath);
		PF_CHECK(Tests, !DuplicateInInvalidProject.has_value());
		PF_CHECK(
			Tests, !DuplicateInInvalidProject && DuplicateInInvalidProject.error().Code == AssetOperationErrorCode::InvalidProjectAssets);
		PF_CHECK(Tests, !std::filesystem::exists(ProjectRoot / CollisionCopyPath));
		std::filesystem::remove(AliasedSource, FileError);
		PF_CHECK(Tests, !FileError);
		std::filesystem::remove(AssetMetadataSerializer::GetSidecarPath(AliasedSource), FileError);
		PF_CHECK(Tests, !FileError);

		std::filesystem::remove(AssetMetadataSerializer::GetSidecarPath(MovedSource), FileError);
		PF_CHECK(Tests, !FileError);
		const auto MissingMetadataOperation =
			AssetOperations::Duplicate(InitialRegistry, ProjectRoot, SourceMetadata->ID, "Assets/Models/untracked.gltf");
		PF_CHECK(Tests, !MissingMetadataOperation.has_value());
		PF_CHECK(
			Tests, !MissingMetadataOperation && MissingMetadataOperation.error().Code == AssetOperationErrorCode::InvalidProjectAssets);
		PF_CHECK(Tests, !std::filesystem::exists(ProjectRoot / "Assets/Models/untracked.gltf"));
	}

	void TestAssetFolderOperations(TestRunner& Tests)
	{
		using namespace PulseForge;
		std::error_code Error;
		const auto ProjectIdentifier = UUID::Generate();
		PF_CHECK(Tests, ProjectIdentifier.has_value());
		if (!ProjectIdentifier) return;

		const std::filesystem::path ProjectRoot =
			std::filesystem::temp_directory_path() / ("PulseForgeAssetFolderOperations-" + ProjectIdentifier->ToString());
		struct ProjectDirectoryCleanup
		{
			std::filesystem::path Path;
			~ProjectDirectoryCleanup()
			{
				std::error_code CleanupError;
				std::filesystem::remove_all(Path, CleanupError);
			}
		} Cleanup{ ProjectRoot };

		std::filesystem::create_directories(ProjectRoot / "Assets" / "Source" / "Nested", Error);
		std::filesystem::create_directories(ProjectRoot / "Assets" / "Destination", Error);
		PF_CHECK(Tests, !Error);
		if (Error) return;

		const auto WriteManagedAsset = [](const std::filesystem::path& Path, std::string_view Contents) -> std::optional<AssetID>
		{
			std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
			if (!Output) return std::nullopt;
			Output.write(Contents.data(), static_cast<std::streamsize>(Contents.size()));
			Output.close();
			if (!Output) return std::nullopt;
			const auto Metadata = AssetMetadataSerializer::CreateForNewAsset(Path);
			return Metadata ? std::optional<AssetID>(Metadata->ID) : std::nullopt;
		};

		const auto RootAsset = WriteManagedAsset(ProjectRoot / "Assets" / "Source" / "root.material", "root");
		const auto NestedAsset = WriteManagedAsset(ProjectRoot / "Assets" / "Source" / "Nested" / "nested.png", "nested");
		PF_CHECK(Tests, RootAsset.has_value() && NestedAsset.has_value());
		if (!RootAsset || !NestedAsset) return;

		AssetRegistry Registry;
		PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());
		PF_CHECK(Tests, Registry.GetAssetCount() == 2);

		const auto SelfMove = AssetOperations::MoveFolder(Registry, ProjectRoot, "Assets/Source", "Assets/Source");
		PF_CHECK(Tests, !SelfMove && SelfMove.error().Code == AssetOperationErrorCode::InvalidPath);
		const auto DescendantMove = AssetOperations::MoveFolder(Registry, ProjectRoot, "Assets/Source", "Assets/Source/Nested");
		PF_CHECK(Tests, !DescendantMove && DescendantMove.error().Code == AssetOperationErrorCode::InvalidPath);
		const auto TraversalMove = AssetOperations::MoveFolder(Registry, ProjectRoot, "Assets/Source", "Assets/../Destination");
		PF_CHECK(Tests, !TraversalMove && TraversalMove.error().Code == AssetOperationErrorCode::InvalidPath);
		PF_CHECK(Tests, std::filesystem::exists(ProjectRoot / "Assets" / "Source" / "root.material"));

		const auto Moved = AssetOperations::MoveFolder(Registry, ProjectRoot, "Assets/Source", "Assets/Destination");
		PF_CHECK(Tests, Moved.has_value());
		PF_CHECK(Tests, Moved && *Moved == std::filesystem::path("Assets/Destination/Source"));
		PF_CHECK(Tests, !std::filesystem::exists(ProjectRoot / "Assets" / "Source"));
		PF_CHECK(Tests, std::filesystem::exists(ProjectRoot / "Assets" / "Destination" / "Source" / "Nested" / "nested.png"));
		const auto MovedRootRecord = Registry.Find(*RootAsset);
		const auto MovedNestedRecord = Registry.Find(*NestedAsset);
		PF_CHECK(Tests,
			MovedRootRecord && MovedRootRecord->ProjectRelativePath == std::filesystem::path("Assets/Destination/Source/root.material"));
		PF_CHECK(Tests, MovedNestedRecord &&
							MovedNestedRecord->ProjectRelativePath == std::filesystem::path("Assets/Destination/Source/Nested/nested.png"));
		PF_CHECK(Tests, MovedRootRecord && MovedRootRecord->ID == *RootAsset);
		PF_CHECK(Tests, MovedNestedRecord && MovedNestedRecord->ID == *NestedAsset);

		std::filesystem::create_directories(ProjectRoot / "Assets" / "Other" / "Source", Error);
		PF_CHECK(Tests, !Error);
		const auto Collision = AssetOperations::MoveFolder(Registry, ProjectRoot, "Assets/Other/Source", "Assets/Destination");
		PF_CHECK(Tests, !Collision && Collision.error().Code == AssetOperationErrorCode::DestinationExists);
		PF_CHECK(Tests, std::filesystem::exists(ProjectRoot / "Assets" / "Destination" / "Source" / "root.material"));

		const auto EmptySource = AssetOperations::MoveFolder(Registry, ProjectRoot, "Assets/Destination/Source", "Assets");
		PF_CHECK(Tests, EmptySource.has_value());
		PF_CHECK(Tests, EmptySource && *EmptySource == std::filesystem::path("Assets/Source"));
		const auto ReturnedToRoot = Registry.Find(*RootAsset);
		PF_CHECK(Tests, ReturnedToRoot && ReturnedToRoot->ProjectRelativePath == std::filesystem::path("Assets/Source/root.material"));

		const std::filesystem::path LinkPath = ProjectRoot / "Assets" / "Linked";
		Error.clear();
		std::filesystem::create_directory_symlink(ProjectRoot / "Assets" / "Destination", LinkPath, Error);
		if (!Error)
		{
			const auto ReparseTarget = AssetOperations::MoveFolder(Registry, ProjectRoot, "Assets/Source", "Assets/Linked");
			PF_CHECK(Tests, !ReparseTarget && ReparseTarget.error().Code == AssetOperationErrorCode::InvalidPath);
			std::filesystem::remove(LinkPath, Error);
			PF_CHECK(Tests, !Error);
		}
	}
} // namespace

void RunAssetOperationsTests(TestRunner& Tests)
{
	TestAssetOperations(Tests);
	TestAssetFolderOperations(Tests);
}
