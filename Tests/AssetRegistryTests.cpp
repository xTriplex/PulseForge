#include "TestAssetSupport.h"

namespace
{
	void TestAssetMetadataAndRegistry(TestRunner& Tests)
	{
		using namespace PulseForge;
		std::error_code FileError;
		const std::filesystem::path TemporaryDirectory = std::filesystem::temp_directory_path(FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError) return;

		const auto ProjectIdentifier = UUID::Generate();
		PF_CHECK(Tests, ProjectIdentifier.has_value());
		if (!ProjectIdentifier) return;

		const std::filesystem::path ProjectRoot = TemporaryDirectory / ("PulseForgeAssetRegistry-" + ProjectIdentifier->ToString());
		struct ProjectDirectoryCleanup
		{
			std::filesystem::path Path;
			~ProjectDirectoryCleanup()
			{
				std::error_code Error;
				std::filesystem::remove_all(Path, Error);
			}
		} Cleanup{ ProjectRoot };

		const std::filesystem::path ShaderDirectory = ProjectRoot / "Assets" / "Shaders";
		const std::filesystem::path ModelDirectory = ProjectRoot / "Assets" / "Models";
		std::filesystem::create_directories(ShaderDirectory, FileError);
		PF_CHECK(Tests, !FileError);
		std::filesystem::create_directories(ModelDirectory, FileError);
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
		const auto HasIssue = [](const AssetRegistryError& Error, AssetRegistryIssueCode Code)
		{
			return std::find_if(Error.Issues.begin(), Error.Issues.end(),
					   [Code](const AssetRegistryIssue& Issue) { return Issue.Code == Code; }) != Error.Issues.end();
		};

		const std::filesystem::path FirstSource = ShaderDirectory / "first.hlsl";
		const std::filesystem::path SecondSource = ShaderDirectory / "second.hlsl";
		const std::filesystem::path DuplicateSource = ModelDirectory / "duplicate.hlsl";
		PF_CHECK(Tests, WriteFile(FirstSource, "first shader source"));
		PF_CHECK(Tests, WriteFile(SecondSource, "second shader source"));
		std::filesystem::copy_file(FirstSource, DuplicateSource, std::filesystem::copy_options::none, FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError) return;

		const auto FirstMetadata = AssetMetadataSerializer::CreateForNewAsset(FirstSource);
		const auto SecondMetadata = AssetMetadataSerializer::CreateForNewAsset(SecondSource);
		const auto DuplicateMetadata = AssetMetadataSerializer::CreateForNewAsset(DuplicateSource);
		PF_CHECK(Tests, FirstMetadata.has_value());
		PF_CHECK(Tests, SecondMetadata.has_value());
		PF_CHECK(Tests, DuplicateMetadata.has_value());
		if (!FirstMetadata || !SecondMetadata || !DuplicateMetadata) return;

		PF_CHECK(Tests, FirstMetadata->Version == AssetMetadataSerializer::CurrentVersion);
		PF_CHECK(Tests, FirstMetadata->ID != SecondMetadata->ID);
		PF_CHECK(Tests, FirstMetadata->ID != DuplicateMetadata->ID);
		PF_CHECK(Tests, AssetMetadataSerializer::GetSidecarPath(FirstSource).filename() == "first.hlsl.meta");
		const auto LoadedFirstMetadata = AssetMetadataSerializer::LoadFromFile(AssetMetadataSerializer::GetSidecarPath(FirstSource));
		PF_CHECK(Tests, LoadedFirstMetadata.has_value());
		PF_CHECK(Tests, LoadedFirstMetadata && LoadedFirstMetadata->ID == FirstMetadata->ID);
		const auto RecreateExistingMetadata = AssetMetadataSerializer::CreateForNewAsset(FirstSource);
		PF_CHECK(Tests, !RecreateExistingMetadata.has_value());
		PF_CHECK(
			Tests, !RecreateExistingMetadata && RecreateExistingMetadata.error().Code == AssetMetadataErrorCode::MetadataAlreadyExists);
		const auto ExistingIdentity = AssetMetadataSerializer::LoadFromFile(AssetMetadataSerializer::GetSidecarPath(FirstSource));
		PF_CHECK(Tests, ExistingIdentity && ExistingIdentity->ID == FirstMetadata->ID);

		AssetRegistry Registry;
		const auto InitialBuild = Registry.Rebuild(ProjectRoot);
		PF_CHECK(Tests, InitialBuild.has_value());
		PF_CHECK(Tests, Registry.GetAssetCount() == 3);
		const std::vector<AssetRecord> RegisteredAssets = Registry.GetAssets();
		PF_CHECK(Tests, RegisteredAssets.size() == 3);
		if (RegisteredAssets.size() != 3) return;
		PF_CHECK(Tests, RegisteredAssets[0].ProjectRelativePath == std::filesystem::path("Assets/Models/duplicate.hlsl"));
		PF_CHECK(Tests, RegisteredAssets[1].ProjectRelativePath == std::filesystem::path("Assets/Shaders/first.hlsl"));
		PF_CHECK(Tests, RegisteredAssets[2].ProjectRelativePath == std::filesystem::path("Assets/Shaders/second.hlsl"));
		const auto FirstRecord = Registry.Find(FirstMetadata->ID);
		PF_CHECK(Tests, FirstRecord.has_value());
		if (!InitialBuild || !FirstRecord) return;
		PF_CHECK(Tests, FirstRecord && FirstRecord->ProjectRelativePath == std::filesystem::path("Assets/Shaders/first.hlsl"));
		PF_CHECK(Tests, !Registry.Find(UUID{}).has_value());

		AssetRegistry ReconstructedRegistry;
		PF_CHECK(Tests, ReconstructedRegistry.Rebuild(ProjectRoot).has_value());
		const auto ReconstructedFirst = ReconstructedRegistry.Find(FirstMetadata->ID);
		PF_CHECK(Tests, ReconstructedFirst && ReconstructedFirst->ProjectRelativePath == FirstRecord->ProjectRelativePath);

		const std::filesystem::path RenamedSource = ModelDirectory / "renamed.hlsl";
		const std::filesystem::path FirstSidecar = AssetMetadataSerializer::GetSidecarPath(FirstSource);
		const std::filesystem::path RenamedSidecar = AssetMetadataSerializer::GetSidecarPath(RenamedSource);
		std::filesystem::rename(FirstSource, RenamedSource, FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError) return;
		std::filesystem::rename(FirstSidecar, RenamedSidecar, FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError) return;
		const auto StaleFirstRecord = Registry.Find(FirstMetadata->ID);
		PF_CHECK(Tests, StaleFirstRecord && StaleFirstRecord->ProjectRelativePath == std::filesystem::path("Assets/Shaders/first.hlsl"));
		const auto RebuiltAfterMove = Registry.Rebuild(ProjectRoot);
		PF_CHECK(Tests, RebuiltAfterMove.has_value());
		const auto MovedRecord = Registry.Find(FirstMetadata->ID);
		PF_CHECK(Tests, MovedRecord && MovedRecord->ProjectRelativePath == std::filesystem::path("Assets/Models/renamed.hlsl"));
		const std::vector<AssetRecord> AssetsAfterMove = Registry.GetAssets();
		const auto MovedRecordInListing = std::find_if(AssetsAfterMove.begin(), AssetsAfterMove.end(),
			[&FirstMetadata](const AssetRecord& Asset) { return Asset.ID == FirstMetadata->ID; });
		PF_CHECK(Tests, MovedRecordInListing != AssetsAfterMove.end() &&
							MovedRecordInListing->ProjectRelativePath == std::filesystem::path("Assets/Models/renamed.hlsl"));

		const std::filesystem::path SecondSidecar = AssetMetadataSerializer::GetSidecarPath(SecondSource);
		const auto OriginalSecondSidecar = ReadFile(SecondSidecar);
		PF_CHECK(Tests, OriginalSecondSidecar.has_value());
		if (!OriginalSecondSidecar || !MovedRecord) return;
		PF_CHECK(Tests, OriginalSecondSidecar->find("\"importSettings\": {}") != std::string::npos);

		std::filesystem::remove(SecondSidecar, FileError);
		PF_CHECK(Tests, !FileError);
		const auto MissingMetadataBuild = Registry.Rebuild(ProjectRoot);
		PF_CHECK(Tests, !MissingMetadataBuild.has_value());
		PF_CHECK(
			Tests, !MissingMetadataBuild.has_value() && HasIssue(MissingMetadataBuild.error(), AssetRegistryIssueCode::MissingMetadata));
		if (!MissingMetadataBuild)
		{
			const auto MissingIssue = std::find_if(MissingMetadataBuild.error().Issues.begin(), MissingMetadataBuild.error().Issues.end(),
				[](const AssetRegistryIssue& Issue) { return Issue.Code == AssetRegistryIssueCode::MissingMetadata; });
			PF_CHECK(Tests, MissingIssue != MissingMetadataBuild.error().Issues.end());
			if (MissingIssue != MissingMetadataBuild.error().Issues.end())
			{
				PF_CHECK(Tests, MissingIssue->ProjectRelativePath == std::filesystem::path("Assets/Shaders/second.hlsl"));
				PF_CHECK(Tests, !MissingIssue->Message.empty());
			}
		}
		PF_CHECK(Tests, !std::filesystem::exists(SecondSidecar));
		PF_CHECK(Tests, Registry.Find(SecondMetadata->ID).has_value());
		PF_CHECK(Tests, WriteFile(SecondSidecar, *OriginalSecondSidecar));

		PF_CHECK(Tests, WriteFile(SecondSidecar, "{ malformed metadata"));
		const auto CorruptMetadataLoad = AssetMetadataSerializer::LoadFromFile(SecondSidecar);
		PF_CHECK(Tests, !CorruptMetadataLoad.has_value());
		const auto CorruptSidecarBeforeCreate = ReadFile(SecondSidecar);
		const auto RecreateCorruptMetadata = AssetMetadataSerializer::CreateForNewAsset(SecondSource);
		PF_CHECK(Tests, !RecreateCorruptMetadata.has_value());
		PF_CHECK(Tests, !RecreateCorruptMetadata && RecreateCorruptMetadata.error().Code == AssetMetadataErrorCode::MetadataAlreadyExists);
		PF_CHECK(Tests, CorruptSidecarBeforeCreate && ReadFile(SecondSidecar) == CorruptSidecarBeforeCreate);
		const auto CorruptMetadataBuild = Registry.Rebuild(ProjectRoot);
		PF_CHECK(Tests, !CorruptMetadataBuild.has_value());
		PF_CHECK(
			Tests, !CorruptMetadataBuild.has_value() && HasIssue(CorruptMetadataBuild.error(), AssetRegistryIssueCode::InvalidMetadata));
		if (!CorruptMetadataBuild)
		{
			const auto InvalidIssue = std::find_if(CorruptMetadataBuild.error().Issues.begin(), CorruptMetadataBuild.error().Issues.end(),
				[](const AssetRegistryIssue& Issue) { return Issue.Code == AssetRegistryIssueCode::InvalidMetadata; });
			PF_CHECK(Tests, InvalidIssue != CorruptMetadataBuild.error().Issues.end());
			if (InvalidIssue != CorruptMetadataBuild.error().Issues.end())
				PF_CHECK(Tests, InvalidIssue->ProjectRelativePath == std::filesystem::path("Assets/Shaders/second.hlsl"));
		}
		PF_CHECK(Tests, Registry.Find(SecondMetadata->ID).has_value());
		PF_CHECK(Tests, WriteFile(SecondSidecar, *OriginalSecondSidecar));

		const std::string UnsupportedVersion =
			"{\"format\":\"PulseForgeAssetMeta\",\"version\":2,\"uuid\":\"" + SecondMetadata->ID.ToString() + "\",\"importSettings\":{}}";
		PF_CHECK(Tests, WriteFile(SecondSidecar, UnsupportedVersion));
		const auto UnsupportedMetadataVersion = AssetMetadataSerializer::LoadFromFile(SecondSidecar);
		PF_CHECK(Tests, !UnsupportedMetadataVersion.has_value());
		PF_CHECK(
			Tests, !UnsupportedMetadataVersion && UnsupportedMetadataVersion.error().Code == AssetMetadataErrorCode::UnsupportedVersion);
		std::string InvalidImportSettings = *OriginalSecondSidecar;
		const size_t ImportSettingsObject = InvalidImportSettings.find("\"importSettings\": {}");
		PF_CHECK(Tests, ImportSettingsObject != std::string::npos);
		if (ImportSettingsObject != std::string::npos)
			InvalidImportSettings.replace(ImportSettingsObject, std::string("\"importSettings\": {}").size(), "\"importSettings\": []");
		PF_CHECK(Tests, WriteFile(SecondSidecar, InvalidImportSettings));
		const auto InvalidImportSettingsLoad = AssetMetadataSerializer::LoadFromFile(SecondSidecar);
		PF_CHECK(Tests, !InvalidImportSettingsLoad.has_value());
		PF_CHECK(Tests, !InvalidImportSettingsLoad && InvalidImportSettingsLoad.error().Code == AssetMetadataErrorCode::InvalidDocument);
		PF_CHECK(Tests, WriteFile(SecondSidecar, *OriginalSecondSidecar));

		std::filesystem::copy_file(RenamedSidecar, SecondSidecar, std::filesystem::copy_options::overwrite_existing, FileError);
		PF_CHECK(Tests, !FileError);
		const auto DuplicateIdentityBuild = Registry.Rebuild(ProjectRoot);
		PF_CHECK(Tests, !DuplicateIdentityBuild.has_value());
		PF_CHECK(Tests,
			!DuplicateIdentityBuild.has_value() && HasIssue(DuplicateIdentityBuild.error(), AssetRegistryIssueCode::DuplicateAssetID));
		if (!DuplicateIdentityBuild)
		{
			const auto DuplicateIssue =
				std::find_if(DuplicateIdentityBuild.error().Issues.begin(), DuplicateIdentityBuild.error().Issues.end(),
					[](const AssetRegistryIssue& Issue) { return Issue.Code == AssetRegistryIssueCode::DuplicateAssetID; });
			PF_CHECK(Tests, DuplicateIssue != DuplicateIdentityBuild.error().Issues.end());
			if (DuplicateIssue != DuplicateIdentityBuild.error().Issues.end())
			{
				PF_CHECK(Tests, DuplicateIssue->ProjectRelativePath == std::filesystem::path("Assets/Shaders/second.hlsl"));
				PF_CHECK(Tests, DuplicateIssue->RelatedProjectRelativePath == std::filesystem::path("Assets/Models/renamed.hlsl"));
				PF_CHECK(Tests, DuplicateIssue->Message.find(FirstMetadata->ID.ToString()) != std::string::npos);
			}
		}
		PF_CHECK(Tests, Registry.Find(SecondMetadata->ID).has_value());
		const auto PreservedFirstRecord = Registry.Find(FirstMetadata->ID);
		PF_CHECK(Tests,
			PreservedFirstRecord && PreservedFirstRecord->ProjectRelativePath == std::filesystem::path("Assets/Models/renamed.hlsl"));
		const auto DuplicateCreation = AssetMetadataSerializer::CreateForNewAsset(SecondSource);
		PF_CHECK(Tests, !DuplicateCreation.has_value());
		PF_CHECK(Tests, !DuplicateCreation && DuplicateCreation.error().Code == AssetMetadataErrorCode::MetadataAlreadyExists);
		PF_CHECK(Tests, WriteFile(SecondSidecar, *OriginalSecondSidecar));
		PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());

		std::filesystem::remove(DuplicateSource, FileError);
		PF_CHECK(Tests, !FileError);
		const auto OrphanedSidecarBuild = Registry.Rebuild(ProjectRoot);
		PF_CHECK(Tests, !OrphanedSidecarBuild.has_value());
		PF_CHECK(
			Tests, !OrphanedSidecarBuild.has_value() && HasIssue(OrphanedSidecarBuild.error(), AssetRegistryIssueCode::MissingSourceAsset));
		PF_CHECK(Tests, Registry.Find(DuplicateMetadata->ID).has_value());

		const std::filesystem::path ReservedSuffixSource = ShaderDirectory / "reserved-suffix";
		PF_CHECK(Tests, WriteFile(ReservedSuffixSource, "regular source asset"));
		const auto ReservedSuffixMetadata = AssetMetadataSerializer::CreateForNewAsset(ReservedSuffixSource);
		PF_CHECK(Tests, ReservedSuffixMetadata.has_value());
		const auto NestedSidecarID = UUID::Generate();
		PF_CHECK(Tests, NestedSidecarID.has_value());
		if (!ReservedSuffixMetadata || !NestedSidecarID) return;

		const std::filesystem::path NestedSidecarPath =
			AssetMetadataSerializer::GetSidecarPath(AssetMetadataSerializer::GetSidecarPath(ReservedSuffixSource));
		const std::string NestedSidecar =
			"{\"format\":\"PulseForgeAssetMeta\",\"version\":1,\"uuid\":\"" + NestedSidecarID->ToString() + "\",\"importSettings\":{}}";
		PF_CHECK(Tests, WriteFile(NestedSidecarPath, NestedSidecar));
		const auto ReservedSuffixBuild = Registry.Rebuild(ProjectRoot);
		PF_CHECK(Tests, !ReservedSuffixBuild.has_value());
		PF_CHECK(
			Tests, !ReservedSuffixBuild.has_value() && HasIssue(ReservedSuffixBuild.error(), AssetRegistryIssueCode::UnsupportedEntry));
	}
} // namespace

void RunAssetRegistryTests(TestRunner& Tests)
{
	TestAssetMetadataAndRegistry(Tests);
}
