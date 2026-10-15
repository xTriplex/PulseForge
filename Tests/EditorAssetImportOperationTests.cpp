#include "TestEditorSupport.h"

namespace
{
	void TestAssetImportOperation(TestRunner& Tests)
	{
		using namespace PulseForge;
		std::error_code FileError;
		const std::filesystem::path TemporaryDirectory = std::filesystem::temp_directory_path(FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError) return;

		const auto ProjectIdentifier = UUID::Generate();
		const auto SourceIdentifier = UUID::Generate();
		PF_CHECK(Tests, ProjectIdentifier && SourceIdentifier);
		if (!ProjectIdentifier || !SourceIdentifier) return;

		const std::filesystem::path ProjectRoot = TemporaryDirectory / ("PulseForgeImportProject-" + ProjectIdentifier->ToString());
		const std::filesystem::path SourceDirectory = TemporaryDirectory / ("PulseForgeImportSource-" + SourceIdentifier->ToString());
		struct TemporaryDirectoryCleanup
		{
			std::filesystem::path Path;
			~TemporaryDirectoryCleanup()
			{
				std::error_code Error;
				std::filesystem::remove_all(Path, Error);
			}
		};
		TemporaryDirectoryCleanup ProjectCleanup{ ProjectRoot };
		TemporaryDirectoryCleanup SourceCleanup{ SourceDirectory };

		std::filesystem::create_directories(ProjectRoot / "Assets" / "Imported", FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError) return;
		std::filesystem::create_directories(SourceDirectory, FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError) return;

		const std::filesystem::path SourceFile = SourceDirectory / "image.bin";
		const std::string SourceContents("source bytes\0with binary data", 29);
		{
			std::ofstream Output(SourceFile, std::ios::binary | std::ios::trunc);
			Output.write(SourceContents.data(), static_cast<std::streamsize>(SourceContents.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}

		AssetRegistry Registry;
		PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());
		PF_CHECK(Tests, Registry.GetAssetCount() == 0);
		const auto Imported = AssetOperations::ImportFile(Registry, ProjectRoot, SourceFile, "Assets/Imported/image.bin");
		PF_CHECK(Tests, Imported.has_value());
		if (!Imported) return;

		const std::filesystem::path ImportedPath = ProjectRoot / "Assets" / "Imported" / "image.bin";
		const std::filesystem::path ImportedSidecar = AssetMetadataSerializer::GetSidecarPath(ImportedPath);
		const auto ImportedMetadata = AssetMetadataSerializer::LoadFromFile(ImportedSidecar);
		PF_CHECK(Tests, ImportedMetadata && ImportedMetadata->ID == Imported->ID);
		PF_CHECK(Tests, ImportedMetadata && ImportedMetadata->Version == AssetMetadataSerializer::CurrentVersion);
		PF_CHECK(Tests, Imported->ProjectRelativePath == std::filesystem::path("Assets/Imported/image.bin"));
		PF_CHECK(Tests, Registry.Find(Imported->ID).has_value());
		PF_CHECK(Tests, std::filesystem::exists(SourceFile));
		const auto ReadFile = [](const std::filesystem::path& Path) -> std::optional<std::string>
		{
			std::ifstream Input(Path, std::ios::binary);
			if (!Input) return std::nullopt;
			std::string Contents{ std::istreambuf_iterator<char>(Input), std::istreambuf_iterator<char>() };
			if (Input.bad()) return std::nullopt;
			return Contents;
		};
		PF_CHECK(Tests, ReadFile(ImportedPath) == SourceContents);
		PF_CHECK(Tests, ReadFile(SourceFile) == SourceContents);

		const auto SecondImport = AssetOperations::ImportFile(Registry, ProjectRoot, SourceFile, "Assets/Imported/second-copy.bin");
		PF_CHECK(Tests, SecondImport.has_value());
		PF_CHECK(Tests, SecondImport && SecondImport->ID != Imported->ID);
		PF_CHECK(Tests, SecondImport && ReadFile(ProjectRoot / SecondImport->ProjectRelativePath) == SourceContents);
		PF_CHECK(Tests, Registry.GetAssetCount() == 2);

		const auto ExistingDestination = AssetOperations::ImportFile(Registry, ProjectRoot, SourceFile, "Assets/Imported/image.bin");
		PF_CHECK(Tests, !ExistingDestination && ExistingDestination.error().Code == AssetOperationErrorCode::DestinationExists);
		PF_CHECK(Tests, ReadFile(ImportedPath) == SourceContents);

		const auto InvalidTraversal = AssetOperations::ImportFile(Registry, ProjectRoot, SourceFile, "Assets/../escaped.bin");
		PF_CHECK(Tests, !InvalidTraversal && InvalidTraversal.error().Code == AssetOperationErrorCode::InvalidPath);
		const auto MissingSource =
			AssetOperations::ImportFile(Registry, ProjectRoot, SourceDirectory / "missing.bin", "Assets/Imported/missing.bin");
		PF_CHECK(Tests, !MissingSource && MissingSource.error().Code == AssetOperationErrorCode::ImportSourceInvalid);
		PF_CHECK(Tests, !std::filesystem::exists(ProjectRoot / "Assets" / "Imported" / "missing.bin"));

		const std::span<const char> BinaryContents(SourceContents.data(), SourceContents.size());
		const auto Created =
			AssetOperations::CreateAssetFromBytes(Registry, ProjectRoot, std::as_bytes(BinaryContents), "Assets/Imported/generated.bin");
		PF_CHECK(Tests, Created.has_value());
		PF_CHECK(Tests, Created && Created->ID != Imported->ID && Created->ID != SecondImport->ID);
		const std::filesystem::path CreatedPath = ProjectRoot / "Assets" / "Imported" / "generated.bin";
		PF_CHECK(Tests, ReadFile(CreatedPath) == SourceContents);
		const auto CreatedMetadata = AssetMetadataSerializer::LoadFromFile(AssetMetadataSerializer::GetSidecarPath(CreatedPath));
		PF_CHECK(Tests, Created && CreatedMetadata && CreatedMetadata->ID == Created->ID);
		PF_CHECK(Tests, Registry.GetAssetCount() == 3);
		PF_CHECK(Tests, Created && Registry.Find(Created->ID).has_value());
		if (Created)
		{
			const auto DuplicateCreation = AssetOperations::CreateAssetFromBytes(
				Registry, ProjectRoot, std::as_bytes(BinaryContents), "Assets/Imported/generated.bin");
			PF_CHECK(Tests, !DuplicateCreation && DuplicateCreation.error().Code == AssetOperationErrorCode::DestinationExists);
			PF_CHECK(Tests, !DuplicateCreation.error().CommittedAsset.has_value());
			PF_CHECK(Tests, ReadFile(CreatedPath) == SourceContents);

			PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());
			PF_CHECK(Tests, Registry.Find(Created->ID).has_value());
		}

		const auto EmptyCreated = AssetOperations::CreateAssetFromBytes(Registry, ProjectRoot, {}, "Assets/Imported/empty.bin");
		PF_CHECK(Tests, EmptyCreated.has_value());
		PF_CHECK(Tests, EmptyCreated && std::filesystem::file_size(ProjectRoot / EmptyCreated->ProjectRelativePath) == 0);

		const std::filesystem::path UntrackedFile = ProjectRoot / "Assets" / "Imported" / "untracked.bin";
		{
			std::ofstream Output(UntrackedFile, std::ios::binary | std::ios::trunc);
			Output << "missing sidecar";
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto RefusedByRegistry = AssetOperations::ImportFile(Registry, ProjectRoot, SourceFile, "Assets/Imported/refused.bin");
		PF_CHECK(Tests, !RefusedByRegistry && RefusedByRegistry.error().Code == AssetOperationErrorCode::InvalidProjectAssets);
		PF_CHECK(Tests, !std::filesystem::exists(ProjectRoot / "Assets" / "Imported" / "refused.bin"));
	}
} // namespace

void RunEditorAssetImportOperationTests(TestRunner& Tests)
{
	TestAssetImportOperation(Tests);
}
