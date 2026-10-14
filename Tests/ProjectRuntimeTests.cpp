#include "TestEditorSupport.h"

namespace
{
	void TestMaterialAssets(TestRunner& Tests)
	{
		using namespace PulseForge;
		const auto TextureAsset = UUID::Generate();
		PF_CHECK(Tests, TextureAsset.has_value());
		if (!TextureAsset)
			return;

		MaterialAssetDesc Description;
		Description.BaseColorTexture = *TextureAsset;
		Description.BaseColorFactor = { 0.8f, 0.6f, 0.4f, 1.0f };
		Description.MetallicFactor = 0.65f;
		Description.RoughnessFactor = 0.25f;
		const auto Serialized = MaterialAssetSerializer::Serialize(Description);
		PF_CHECK(Tests, Serialized.has_value());
		if (!Serialized)
			return;
		const auto Deserialized = MaterialAssetSerializer::Deserialize(*Serialized);
		PF_CHECK(Tests, Deserialized && Deserialized->BaseColorTexture == Description.BaseColorTexture);
		PF_CHECK(Tests, Deserialized && glm::all(glm::equal(Deserialized->BaseColorFactor, Description.BaseColorFactor)));
		PF_CHECK(Tests, Deserialized && Deserialized->MetallicFactor == Description.MetallicFactor &&
			Deserialized->RoughnessFactor == Description.RoughnessFactor);
		PF_CHECK(Tests, Serialized && Serialized->find("\"version\": 4") != std::string::npos);
		const std::string Version1Document = "{\"format\":\"PulseForgeMaterial\",\"version\":1,\"baseColorTexture\":\"" +
			TextureAsset->ToString() + "\",\"baseColorFactor\":[1,1,1,1]}";
		const auto Version1Material = MaterialAssetSerializer::Deserialize(Version1Document);
		PF_CHECK(Tests, Version1Material && Version1Material->MetallicFactor == 0.0f &&
			Version1Material->RoughnessFactor == 1.0f);
		const std::string Version2Document = "{\"format\":\"PulseForgeMaterial\",\"version\":2,\"baseColorTexture\":\"" +
			TextureAsset->ToString() + "\",\"baseColorFactor\":[1,1,1,1],\"metallicFactor\":0.4,\"roughnessFactor\":0.6}";
		const auto Version2Material = MaterialAssetSerializer::Deserialize(Version2Document);
		PF_CHECK(Tests, Version2Material && Version2Material->MetallicFactor == 0.4f &&
			Version2Material->RoughnessFactor == 0.6f && Version2Material->NormalTexture.IsNil());
		const std::string Version3Document = "{\"format\":\"PulseForgeMaterial\",\"version\":3,\"baseColorFactor\":[1,1,1,1],\"baseColorTexture\":null,\"metallicFactor\":0.4,\"roughnessFactor\":0.6}";
		const auto Version3Material = MaterialAssetSerializer::Deserialize(Version3Document);
		PF_CHECK(Tests, Version3Material && Version3Material->AlphaMode == MaterialAlphaMode::Opaque &&
			Version3Material->AlphaCutoff == 0.5f && !Version3Material->DoubleSided);
		MaterialAssetDesc TexturedDescription = Description;
		TexturedDescription.BaseColorTexture = {};
		TexturedDescription.MetallicRoughnessTexture = *TextureAsset;
		TexturedDescription.NormalTexture = *TextureAsset;
		TexturedDescription.OcclusionTexture = *TextureAsset;
		TexturedDescription.EmissiveTexture = *TextureAsset;
		TexturedDescription.NormalScale = 0.75f;
		TexturedDescription.OcclusionStrength = 0.35f;
		TexturedDescription.EmissiveFactor = { 0.5f, 1.25f, 2.0f };
		TexturedDescription.AlphaMode = MaterialAlphaMode::Mask;
		TexturedDescription.AlphaCutoff = 0.42f;
		TexturedDescription.DoubleSided = true;
		const auto TexturedSerialized = MaterialAssetSerializer::Serialize(TexturedDescription);
		PF_CHECK(Tests, TexturedSerialized.has_value());
		if (!TexturedSerialized)
			return;
		const auto TexturedDeserialized = MaterialAssetSerializer::Deserialize(*TexturedSerialized);
		PF_CHECK(Tests, TexturedDeserialized && TexturedDeserialized->BaseColorTexture.IsNil() &&
			TexturedDeserialized->MetallicRoughnessTexture == TexturedDescription.MetallicRoughnessTexture &&
			TexturedDeserialized->NormalTexture == TexturedDescription.NormalTexture &&
			TexturedDeserialized->OcclusionTexture == TexturedDescription.OcclusionTexture &&
			TexturedDeserialized->EmissiveTexture == TexturedDescription.EmissiveTexture);
		PF_CHECK(Tests, TexturedDeserialized && TexturedDeserialized->NormalScale == 0.75f &&
			TexturedDeserialized->OcclusionStrength == 0.35f &&
			glm::all(glm::equal(TexturedDeserialized->EmissiveFactor, TexturedDescription.EmissiveFactor)));
		PF_CHECK(Tests, TexturedDeserialized && TexturedDeserialized->AlphaMode == MaterialAlphaMode::Mask &&
			TexturedDeserialized->AlphaCutoff == 0.42f && TexturedDeserialized->DoubleSided);
		MaterialAssetDesc BlendDescription = Description;
		BlendDescription.AlphaMode = MaterialAlphaMode::Blend;
		const auto BlendSerialized = MaterialAssetSerializer::Serialize(BlendDescription);
		PF_CHECK(Tests, BlendSerialized.has_value());
		if (BlendSerialized)
		{
			const auto BlendDeserialized = MaterialAssetSerializer::Deserialize(*BlendSerialized);
			PF_CHECK(Tests, BlendDeserialized && BlendDeserialized->AlphaMode == MaterialAlphaMode::Blend);
		}
		PF_CHECK(Tests, !MaterialAssetSerializer::Deserialize(
			"{\"format\":\"PulseForgeMaterial\",\"version\":4,\"baseColorFactor\":[1,1,1,1],\"alphaMode\":\"HASH\"}"));
		PF_CHECK(Tests, !MaterialAssetSerializer::Deserialize(
			"{\"format\":\"PulseForgeMaterial\",\"version\":3,\"baseColorFactor\":[1,1,1,1],\"normalTexture\":7}"));
		std::string InvalidMetallicDocument = *Serialized;
		const size_t MetallicField = InvalidMetallicDocument.find("\"metallicFactor\":");
		const size_t MetallicValue = MetallicField == std::string::npos ? MetallicField :
			InvalidMetallicDocument.find_first_not_of(" :", MetallicField + std::string("\"metallicFactor\"").size());
		const size_t MetallicEnd = MetallicValue == std::string::npos ? MetallicValue :
			InvalidMetallicDocument.find_first_of(",\n", MetallicValue);
		PF_CHECK(Tests, MetallicValue != std::string::npos && MetallicEnd != std::string::npos);
		if (MetallicEnd != std::string::npos)
			InvalidMetallicDocument.replace(MetallicValue, MetallicEnd - MetallicValue, "-0.1");
		PF_CHECK(Tests, !MaterialAssetSerializer::Deserialize(InvalidMetallicDocument));

		MaterialAssetDesc MissingTexture = Description;
		MissingTexture.BaseColorTexture = {};
		PF_CHECK(Tests, ValidateMaterialAssetDescription(MissingTexture).has_value());
		MaterialAssetDesc InvalidFactor = Description;
		InvalidFactor.BaseColorFactor.g = std::numeric_limits<float>::quiet_NaN();
		PF_CHECK(Tests, !ValidateMaterialAssetDescription(InvalidFactor));
		InvalidFactor.BaseColorFactor = Description.BaseColorFactor;
		InvalidFactor.BaseColorFactor.r = 1.1f;
		PF_CHECK(Tests, !ValidateMaterialAssetDescription(InvalidFactor));
		InvalidFactor = Description;
		InvalidFactor.MetallicFactor = -0.01f;
		PF_CHECK(Tests, !ValidateMaterialAssetDescription(InvalidFactor));
		InvalidFactor.MetallicFactor = 1.01f;
		PF_CHECK(Tests, !ValidateMaterialAssetDescription(InvalidFactor));
		InvalidFactor = Description;
		InvalidFactor.RoughnessFactor = std::numeric_limits<float>::infinity();
		PF_CHECK(Tests, !ValidateMaterialAssetDescription(InvalidFactor));
		InvalidFactor = Description;
		InvalidFactor.NormalScale = -0.01f;
		PF_CHECK(Tests, !ValidateMaterialAssetDescription(InvalidFactor));
		InvalidFactor.NormalScale = 4.01f;
		PF_CHECK(Tests, !ValidateMaterialAssetDescription(InvalidFactor));
		InvalidFactor = Description;
		InvalidFactor.OcclusionStrength = std::numeric_limits<float>::quiet_NaN();
		PF_CHECK(Tests, !ValidateMaterialAssetDescription(InvalidFactor));
		InvalidFactor = Description;
		InvalidFactor.AlphaCutoff = std::numeric_limits<float>::infinity();
		PF_CHECK(Tests, !ValidateMaterialAssetDescription(InvalidFactor));
		InvalidFactor.AlphaCutoff = -0.01f;
		PF_CHECK(Tests, !ValidateMaterialAssetDescription(InvalidFactor));
		InvalidFactor = Description;
		InvalidFactor.AlphaMode = static_cast<MaterialAlphaMode>(0xff);
		PF_CHECK(Tests, !ValidateMaterialAssetDescription(InvalidFactor));
		InvalidFactor = Description;
		InvalidFactor.EmissiveFactor.g = -0.01f;
		PF_CHECK(Tests, !ValidateMaterialAssetDescription(InvalidFactor));
		InvalidFactor.EmissiveFactor = { 10000.1f, 0.0f, 0.0f };
		PF_CHECK(Tests, !ValidateMaterialAssetDescription(InvalidFactor));
		PF_CHECK(Tests, !MaterialAssetSerializer::Deserialize("{invalid"));
		PF_CHECK(Tests, !MaterialAssetSerializer::Deserialize("{\"format\":\"OtherMaterial\",\"version\":1}"));
		PF_CHECK(Tests, !MaterialAssetSerializer::Deserialize("{\"format\":\"PulseForgeMaterial\",\"version\":99}"));

		std::error_code FileError;
		const std::filesystem::path TemporaryDirectory = std::filesystem::temp_directory_path(FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError)
			return;
		const auto ProjectIdentifier = UUID::Generate();
		PF_CHECK(Tests, ProjectIdentifier.has_value());
		if (!ProjectIdentifier)
			return;

		const std::filesystem::path ProjectRoot = TemporaryDirectory / ("PulseForgeMaterials-" + ProjectIdentifier->ToString());
		struct ProjectCleanup
		{
			std::filesystem::path Path;
			~ProjectCleanup()
			{
				std::error_code Error;
				std::filesystem::remove_all(Path, Error);
			}
		} Cleanup{ ProjectRoot };
		const std::filesystem::path MaterialsDirectory = ProjectRoot / "Assets" / "Materials";
		const std::filesystem::path TexturesDirectory = ProjectRoot / "Assets" / "Textures";
		std::filesystem::create_directories(MaterialsDirectory, FileError);
		PF_CHECK(Tests, !FileError);
		std::filesystem::create_directories(TexturesDirectory, FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError)
			return;

		const std::filesystem::path TexturePath = TexturesDirectory / "base-color.png";
		{
			std::ofstream Output(TexturePath, std::ios::binary | std::ios::trunc);
			Output << "texture placeholder";
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto TextureMetadata = AssetMetadataSerializer::CreateForNewAsset(TexturePath);
		PF_CHECK(Tests, TextureMetadata.has_value());
		if (!TextureMetadata)
			return;
		Description.BaseColorTexture = TextureMetadata->ID;

		AssetRegistry Registry;
		PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());
		const auto Created = MaterialAssetService::Create(
			Registry,
			ProjectRoot,
			"Assets/Materials/validation.material",
			Description);
		PF_CHECK(Tests, Created.has_value());
		PF_CHECK(Tests, Created && Created->ID != TextureMetadata->ID);
		if (!Created)
			return;

		const std::filesystem::path MaterialPath = ProjectRoot / "Assets" / "Materials" / "validation.material";
		const auto Metadata = AssetMetadataSerializer::LoadFromFile(AssetMetadataSerializer::GetSidecarPath(MaterialPath));
		PF_CHECK(Tests, Metadata && Metadata->ID == Created->ID);
		PF_CHECK(Tests, Registry.Find(Created->ID).has_value());
		const auto Loaded = MaterialAssetService::Load(Created->ID, ProjectRoot, Registry);
		PF_CHECK(Tests, Loaded && Loaded->BaseColorTexture == Description.BaseColorTexture);
		PF_CHECK(Tests, Loaded && glm::all(glm::equal(Loaded->BaseColorFactor, Description.BaseColorFactor)));
		PF_CHECK(Tests, Loaded && std::abs(Loaded->MetallicFactor - Description.MetallicFactor) < 1.0e-5f);
		PF_CHECK(Tests, Loaded && std::abs(Loaded->RoughnessFactor - Description.RoughnessFactor) < 1.0e-5f);

		MaterialAssetCache Cache(ProjectRoot, Registry);
		const auto Cached = Cache.GetOrLoad(Created->ID);
		const auto CachedAgain = Cache.GetOrLoad(Created->ID);
		PF_CHECK(Tests, Cached && CachedAgain && &Cached->get() == &CachedAgain->get());
		PF_CHECK(Tests, Cache.GetLoadedCount() == 1);

		MaterialAssetDesc Updated = Description;
		Updated.BaseColorFactor = { 0.3f, 0.5f, 0.7f, 1.0f };
		PF_CHECK(Tests, MaterialAssetService::Save(Created->ID, ProjectRoot, Registry, Updated).has_value());
		const auto PreservedMetadata = AssetMetadataSerializer::LoadFromFile(
			AssetMetadataSerializer::GetSidecarPath(MaterialPath));
		PF_CHECK(Tests, PreservedMetadata && PreservedMetadata->ID == Created->ID);
		PF_CHECK(Tests, Cached && glm::all(glm::equal(Cached->get().BaseColorFactor, Description.BaseColorFactor)));
		Cache.Clear();
		const auto Reloaded = Cache.GetOrLoad(Created->ID);
		PF_CHECK(Tests, Reloaded && glm::all(glm::equal(Reloaded->get().BaseColorFactor, Updated.BaseColorFactor)));

		const auto Moved = AssetOperations::Move(Registry, ProjectRoot, Created->ID, "Assets/Materials/renamed.material");
		PF_CHECK(Tests, Moved && Moved->ID == Created->ID);
		const auto LoadedAfterMove = MaterialAssetService::Load(Created->ID, ProjectRoot, Registry);
		PF_CHECK(Tests, LoadedAfterMove && glm::all(glm::equal(LoadedAfterMove->BaseColorFactor, Updated.BaseColorFactor)));

		const auto InvalidExtension = MaterialAssetService::Create(
			Registry,
			ProjectRoot,
			"Assets/Materials/invalid.json",
			Description);
		PF_CHECK(Tests, !InvalidExtension && InvalidExtension.error().Code == MaterialAssetErrorCode::UnsupportedAssetType);
		const auto Duplicate = MaterialAssetService::Create(
			Registry,
			ProjectRoot,
			"Assets/Materials/renamed.material",
			Description);
		PF_CHECK(Tests, !Duplicate && Duplicate.error().Code == MaterialAssetErrorCode::AssetOperationFailed);

		const std::filesystem::path WrongTypePath = MaterialsDirectory / "not-material.txt";
		{
			std::ofstream Output(WrongTypePath, std::ios::binary | std::ios::trunc);
			Output << "not a material";
		}
		const auto WrongTypeMetadata = AssetMetadataSerializer::CreateForNewAsset(WrongTypePath);
		PF_CHECK(Tests, WrongTypeMetadata.has_value());
		const std::filesystem::path BrokenPath = MaterialsDirectory / "broken.material";
		{
			std::ofstream Output(BrokenPath, std::ios::binary | std::ios::trunc);
			Output << "{broken";
		}
		const auto BrokenMetadata = AssetMetadataSerializer::CreateForNewAsset(BrokenPath);
		PF_CHECK(Tests, BrokenMetadata.has_value());
		PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());
		if (WrongTypeMetadata)
		{
			const auto WrongType = MaterialAssetService::Load(WrongTypeMetadata->ID, ProjectRoot, Registry);
			PF_CHECK(Tests, !WrongType && WrongType.error().Code == MaterialAssetErrorCode::UnsupportedAssetType);
		}
		if (BrokenMetadata)
		{
			const auto Broken = MaterialAssetService::Load(BrokenMetadata->ID, ProjectRoot, Registry);
			PF_CHECK(Tests, !Broken && Broken.error().Code == MaterialAssetErrorCode::InvalidDocument);
		}
	}

	void TestProjectFiles(TestRunner& Tests)
	{
		using namespace PulseForge;
		const auto ProjectIdentifier = UUID::Generate();
		PF_CHECK(Tests, ProjectIdentifier.has_value());
		if (!ProjectIdentifier)
			return;

		std::error_code FileError;
		const std::filesystem::path TemporaryDirectory = std::filesystem::temp_directory_path(FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError)
			return;

		const std::filesystem::path Root = TemporaryDirectory / ("PulseForgeProject-" + ProjectIdentifier->ToString());
		const std::filesystem::path RelocatedRoot = Root.parent_path() / (Root.filename().string() + "-relocated");
		struct ProjectCleanup
		{
			std::filesystem::path First;
			std::filesystem::path Second;
			~ProjectCleanup()
			{
				std::error_code Error;
				std::filesystem::remove_all(First, Error);
				Error.clear();
				std::filesystem::remove_all(Second, Error);
			}
		} Cleanup{ Root, RelocatedRoot };

		const std::filesystem::path ProjectFile = Root / "Example.pfproj";
		auto Created = Project::Create(ProjectFile, "Example Project");
		PF_CHECK(Tests, Created.has_value());
		if (!Created)
			return;
		PF_CHECK(Tests, Created->GetDescription().Name == "Example Project");
		PF_CHECK(Tests, !Created->GetDescription().StartScene.has_value());
		PF_CHECK(Tests, std::filesystem::is_directory(Root / "Assets"));
		PF_CHECK(Tests, !Project::Create(ProjectFile, "Duplicate Project"));
		PF_CHECK(Tests, !ProjectSerializer::Deserialize("{broken"));
		PF_CHECK(Tests, !ProjectSerializer::Deserialize("{\"format\":\"PulseForgeProject\",\"version\":99}"));

		const std::filesystem::path ScenesDirectory = Root / "Assets" / "Scenes";
		std::filesystem::create_directories(ScenesDirectory, FileError);
		PF_CHECK(Tests, !FileError);
		const std::filesystem::path ScenePath = ScenesDirectory / "start.scene";
		{
			std::ofstream Output(ScenePath, std::ios::binary | std::ios::trunc);
			Output << "{}";
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto SceneMetadata = AssetMetadataSerializer::CreateForNewAsset(ScenePath);
		PF_CHECK(Tests, SceneMetadata.has_value());
		if (!SceneMetadata)
			return;
		PF_CHECK(Tests, Created->GetAssetRegistry().Rebuild(Root).has_value());
		PF_CHECK(Tests, Created->SetStartScene(SceneMetadata->ID).has_value());
		PF_CHECK(Tests, Created->GetDescription().StartScene == SceneMetadata->ID);

		const std::filesystem::path WrongTypePath = Root / "Assets" / "not-a-scene.txt";
		{
			std::ofstream Output(WrongTypePath, std::ios::binary | std::ios::trunc);
			Output << "asset";
		}
		const auto WrongTypeMetadata = AssetMetadataSerializer::CreateForNewAsset(WrongTypePath);
		PF_CHECK(Tests, WrongTypeMetadata.has_value());
		PF_CHECK(Tests, Created->GetAssetRegistry().Rebuild(Root).has_value());
		if (WrongTypeMetadata)
		{
			const auto WrongType = Created->SetStartScene(WrongTypeMetadata->ID);
			PF_CHECK(Tests, !WrongType && WrongType.error().Code == ProjectErrorCode::StartSceneWrongType);
			PF_CHECK(Tests, Created->GetDescription().StartScene == SceneMetadata->ID);
		}

		Created = Project::Open(ProjectFile);
		PF_CHECK(Tests, Created && Created->GetDescription().StartScene == SceneMetadata->ID);
		if (!Created)
			return;
		std::filesystem::rename(Root, RelocatedRoot, FileError);
		PF_CHECK(Tests, !FileError);
		const auto RelocatedProject = Project::Open(RelocatedRoot / "Example.pfproj");
		PF_CHECK(Tests, RelocatedProject.has_value());
		PF_CHECK(Tests, RelocatedProject && RelocatedProject->GetAssetRegistry().Find(SceneMetadata->ID).has_value());
		PF_CHECK(Tests, RelocatedProject && RelocatedProject->GetRootPath() == RelocatedRoot);
	}

	void TestEditorCommandLineAndStartupProjectLoading(TestRunner& Tests)
	{
		using namespace PulseForge;
		using namespace PulseForgeEditor;
		const auto Identifier = UUID::Generate();
		PF_CHECK(Tests, Identifier.has_value());
		if (!Identifier)
			return;

		std::error_code FileError;
		const std::filesystem::path TemporaryDirectory = std::filesystem::temp_directory_path(FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError)
			return;

		const std::filesystem::path Root =
			TemporaryDirectory / ("PulseForge CLI Test " + Identifier->ToString());
		struct DirectoryCleanup
		{
			std::filesystem::path Path;
			~DirectoryCleanup()
			{
				std::error_code Error;
				std::filesystem::remove_all(Path, Error);
			}
		} Cleanup{ Root };

		std::filesystem::create_directories(Root, FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError)
			return;

		const std::filesystem::path ProjectFile = Root / "Project With Spaces.pfproj";
		auto Created = Project::Create(ProjectFile, "CLI Project With Spaces");
		PF_CHECK(Tests, Created.has_value());
		if (!Created)
			return;

		const std::span<const std::string_view> NoArguments;
		const auto DefaultOptions = ParseEditorCommandLine(NoArguments, Root);
		PF_CHECK(Tests, DefaultOptions.has_value());
		PF_CHECK(Tests, DefaultOptions && !DefaultOptions->ProjectFile.has_value());
		PF_CHECK(Tests, DefaultOptions && !DefaultOptions->EnableDocumentHostTestFixture);
		PF_CHECK(Tests, DefaultOptions && !DefaultOptions->EnableDocumentHostResetLayoutTest);
		PF_CHECK(Tests, DefaultOptions && !DefaultOptions->EnableDocumentHostTabTearOffTest);
		PF_CHECK(Tests, DefaultOptions && !DefaultOptions->EnableDocumentHostTabReattachTest);
		PF_CHECK(Tests, DefaultOptions && !DefaultOptions->EnableDocumentHostTabReorderTest);
		PF_CHECK(Tests, DefaultOptions && !DefaultOptions->EnableDocumentHostPresentationFailureTest);
		PF_CHECK(Tests, DefaultOptions && !DefaultOptions->EnableDocumentHostAcquireOutOfDateTest);
		PF_CHECK(Tests, DefaultOptions && !DefaultOptions->EnableDocumentHostImageAllocationFailureTest);
		PF_CHECK(Tests, DefaultOptions && !DefaultOptions->ProjectOpenRenderTestFile);

		const std::string AbsoluteProjectPath = ProjectFile.string();
		const std::array ProjectOpenRenderTestArguments = {
			std::string_view("--project-open-render-test"), std::string_view(AbsoluteProjectPath) };
		const auto ProjectOpenRenderTestOptions = ParseEditorCommandLine(ProjectOpenRenderTestArguments, Root);
		PF_CHECK(Tests, ProjectOpenRenderTestOptions.has_value());
		PF_CHECK(Tests, ProjectOpenRenderTestOptions &&
			ProjectOpenRenderTestOptions->ProjectOpenRenderTestFile == ProjectFile);
		const std::array MissingProjectOpenRenderTestValue = {
			std::string_view("--project-open-render-test") };
		const auto MissingProjectOpenRenderTestOptions =
			ParseEditorCommandLine(MissingProjectOpenRenderTestValue, Root);
		PF_CHECK(Tests, !MissingProjectOpenRenderTestOptions);
		PF_CHECK(Tests, !MissingProjectOpenRenderTestOptions &&
			MissingProjectOpenRenderTestOptions.error().Code == EditorCommandLineErrorCode::MissingProjectPath);
		const std::array StartupAndProjectOpenRenderTestArguments = {
			std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--project-open-render-test"), std::string_view(AbsoluteProjectPath) };
		const auto StartupAndProjectOpenRenderTestOptions =
			ParseEditorCommandLine(StartupAndProjectOpenRenderTestArguments, Root);
		PF_CHECK(Tests, StartupAndProjectOpenRenderTestOptions.has_value());
		PF_CHECK(Tests, StartupAndProjectOpenRenderTestOptions &&
			StartupAndProjectOpenRenderTestOptions->ProjectFile == ProjectFile &&
			StartupAndProjectOpenRenderTestOptions->ProjectOpenRenderTestFile == ProjectFile);
		const std::array DuplicateProjectOpenRenderTestArguments = {
			std::string_view("--project-open-render-test"), std::string_view(AbsoluteProjectPath),
			std::string_view("--project-open-render-test"), std::string_view(AbsoluteProjectPath) };
		const auto DuplicateProjectOpenRenderTestOptions =
			ParseEditorCommandLine(DuplicateProjectOpenRenderTestArguments, Root);
		PF_CHECK(Tests, !DuplicateProjectOpenRenderTestOptions);
		PF_CHECK(Tests, !DuplicateProjectOpenRenderTestOptions &&
			DuplicateProjectOpenRenderTestOptions.error().Code == EditorCommandLineErrorCode::DuplicateOption);
		const std::array AbsoluteArguments = { std::string_view("--project"), std::string_view(AbsoluteProjectPath) };
		const auto AbsoluteOptions = ParseEditorCommandLine(AbsoluteArguments, Root);
		PF_CHECK(Tests, AbsoluteOptions.has_value());
		PF_CHECK(Tests, AbsoluteOptions && AbsoluteOptions->ProjectFile == ProjectFile);

		const std::string RelativeProjectPath = ProjectFile.filename().string();
		const std::array RelativeArguments = { std::string_view("--project"), std::string_view(RelativeProjectPath) };
		const auto RelativeOptions = ParseEditorCommandLine(RelativeArguments, Root);
		PF_CHECK(Tests, RelativeOptions.has_value());
		PF_CHECK(Tests, RelativeOptions && RelativeOptions->ProjectFile == ProjectFile);

		const std::array HostFixtureArguments = {
			std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-fixture") };
		const auto HostFixtureOptions = ParseEditorCommandLine(HostFixtureArguments, Root);
		PF_CHECK(Tests, HostFixtureOptions.has_value());
		PF_CHECK(Tests, HostFixtureOptions && HostFixtureOptions->EnableDocumentHostTestFixture);
		const std::array HostFixtureWithoutProject = { std::string_view("--document-host-test-fixture") };
		const auto HostFixtureWithoutProjectResult = ParseEditorCommandLine(HostFixtureWithoutProject, Root);
		PF_CHECK(Tests, !HostFixtureWithoutProjectResult);
		PF_CHECK(Tests, !HostFixtureWithoutProjectResult &&
			HostFixtureWithoutProjectResult.error().Code == EditorCommandLineErrorCode::MissingProjectPath);
		const std::array DocumentWorkspaceSwitchFixtureArguments = {
			std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-fixture"),
			std::string_view("--document-workspace-test-switch") };
		const auto DocumentWorkspaceSwitchFixtureOptions =
			ParseEditorCommandLine(DocumentWorkspaceSwitchFixtureArguments, Root);
		PF_CHECK(Tests, DocumentWorkspaceSwitchFixtureOptions.has_value());
		PF_CHECK(Tests, DocumentWorkspaceSwitchFixtureOptions &&
			DocumentWorkspaceSwitchFixtureOptions->EnableDocumentWorkspaceSwitchTest);
		const std::array DocumentWorkspaceSwitchWithoutFixture = {
			std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-workspace-test-switch") };
		const auto DocumentWorkspaceSwitchWithoutFixtureOptions =
			ParseEditorCommandLine(DocumentWorkspaceSwitchWithoutFixture, Root);
		PF_CHECK(Tests, !DocumentWorkspaceSwitchWithoutFixtureOptions);
		const std::array DuplicateHostFixtureArguments = {
			std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-fixture"),
			std::string_view("--document-host-test-fixture") };
		const auto DuplicateHostFixtureOptions = ParseEditorCommandLine(DuplicateHostFixtureArguments, Root);
		PF_CHECK(Tests, !DuplicateHostFixtureOptions);
		PF_CHECK(Tests, !DuplicateHostFixtureOptions &&
			DuplicateHostFixtureOptions.error().Code == EditorCommandLineErrorCode::DuplicateOption);
		const std::array ResetLayoutFixtureArguments = {
			std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-fixture"),
			std::string_view("--document-host-reset-layout-test") };
		const auto ResetLayoutFixtureOptions = ParseEditorCommandLine(ResetLayoutFixtureArguments, Root);
		PF_CHECK(Tests, ResetLayoutFixtureOptions.has_value());
		PF_CHECK(Tests, ResetLayoutFixtureOptions && ResetLayoutFixtureOptions->EnableDocumentHostResetLayoutTest);
		const std::array ResetLayoutWithoutFixture = {
			std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-reset-layout-test") };
		const auto ResetLayoutWithoutFixtureOptions = ParseEditorCommandLine(ResetLayoutWithoutFixture, Root);
		PF_CHECK(Tests, !ResetLayoutWithoutFixtureOptions);
		PF_CHECK(Tests, !ResetLayoutWithoutFixtureOptions &&
			ResetLayoutWithoutFixtureOptions.error().Code == EditorCommandLineErrorCode::UnexpectedArgument);
		const std::array TabTearOffFixtureArguments = {
			std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-fixture"),
			std::string_view("--document-host-test-tab-tear-off") };
		const auto TabTearOffFixtureOptions = ParseEditorCommandLine(TabTearOffFixtureArguments, Root);
		PF_CHECK(Tests, TabTearOffFixtureOptions && TabTearOffFixtureOptions->EnableDocumentHostTabTearOffTest);
		const std::array TabTearOffWithoutFixture = {
			std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-tab-tear-off") };
		const auto TabTearOffWithoutFixtureOptions = ParseEditorCommandLine(TabTearOffWithoutFixture, Root);
		PF_CHECK(Tests, !TabTearOffWithoutFixtureOptions &&
			TabTearOffWithoutFixtureOptions.error().Code == EditorCommandLineErrorCode::UnexpectedArgument);
		const std::array TabReattachFixtureArguments = {
			std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-fixture"),
			std::string_view("--document-host-test-tab-reattach") };
		const auto TabReattachFixtureOptions = ParseEditorCommandLine(TabReattachFixtureArguments, Root);
		PF_CHECK(Tests, TabReattachFixtureOptions && TabReattachFixtureOptions->EnableDocumentHostTabReattachTest);
		const std::array TabReattachWithoutFixture = {
			std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-tab-reattach") };
		const auto TabReattachWithoutFixtureOptions = ParseEditorCommandLine(TabReattachWithoutFixture, Root);
		PF_CHECK(Tests, !TabReattachWithoutFixtureOptions &&
			TabReattachWithoutFixtureOptions.error().Code == EditorCommandLineErrorCode::UnexpectedArgument);
		const std::array ConflictingTabTransferFixtures = {
			std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-fixture"),
			std::string_view("--document-host-test-tab-tear-off"),
			std::string_view("--document-host-test-tab-reattach") };
		const auto ConflictingTabTransferOptions = ParseEditorCommandLine(ConflictingTabTransferFixtures, Root);
		PF_CHECK(Tests, !ConflictingTabTransferOptions &&
			ConflictingTabTransferOptions.error().Code == EditorCommandLineErrorCode::UnexpectedArgument);
		const std::array TabReorderFixtureArguments = {
			std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-fixture"),
			std::string_view("--document-host-test-tab-reorder") };
		const auto TabReorderFixtureOptions = ParseEditorCommandLine(TabReorderFixtureArguments, Root);
		PF_CHECK(Tests, TabReorderFixtureOptions && TabReorderFixtureOptions->EnableDocumentHostTabReorderTest);
		const std::array TabReorderWithoutFixture = {
			std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-tab-reorder") };
		const auto TabReorderWithoutFixtureOptions = ParseEditorCommandLine(TabReorderWithoutFixture, Root);
		PF_CHECK(Tests, !TabReorderWithoutFixtureOptions &&
			TabReorderWithoutFixtureOptions.error().Code == EditorCommandLineErrorCode::UnexpectedArgument);
		const std::array DuplicateTabReorderFixture = {
			std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-fixture"),
			std::string_view("--document-host-test-tab-reorder"),
			std::string_view("--document-host-test-tab-reorder") };
		const auto DuplicateTabReorderOptions = ParseEditorCommandLine(DuplicateTabReorderFixture, Root);
		PF_CHECK(Tests, !DuplicateTabReorderOptions &&
			DuplicateTabReorderOptions.error().Code == EditorCommandLineErrorCode::DuplicateOption);
		const std::array ConflictingTabReorderFixture = {
			std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-fixture"),
			std::string_view("--document-host-test-tab-reattach"),
			std::string_view("--document-host-test-tab-reorder") };
		const auto ConflictingTabReorderOptions = ParseEditorCommandLine(ConflictingTabReorderFixture, Root);
		PF_CHECK(Tests, !ConflictingTabReorderOptions &&
			ConflictingTabReorderOptions.error().Code == EditorCommandLineErrorCode::UnexpectedArgument);
		const std::array PresentationFailureFixtureArguments = {
			std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-fixture"),
			std::string_view("--document-host-test-presentation-failure") };
		const auto PresentationFailureFixtureOptions = ParseEditorCommandLine(PresentationFailureFixtureArguments, Root);
		PF_CHECK(Tests, PresentationFailureFixtureOptions &&
			PresentationFailureFixtureOptions->EnableDocumentHostPresentationFailureTest);
		const std::array PresentationFailureWithoutFixture = {
			std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-presentation-failure") };
		const auto PresentationFailureWithoutFixtureOptions = ParseEditorCommandLine(PresentationFailureWithoutFixture, Root);
		PF_CHECK(Tests, !PresentationFailureWithoutFixtureOptions &&
			PresentationFailureWithoutFixtureOptions.error().Code == EditorCommandLineErrorCode::UnexpectedArgument);
		const std::array DuplicatePresentationFailureFixture = {
			std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-fixture"),
			std::string_view("--document-host-test-presentation-failure"),
			std::string_view("--document-host-test-presentation-failure") };
		const auto DuplicatePresentationFailureOptions = ParseEditorCommandLine(DuplicatePresentationFailureFixture, Root);
		PF_CHECK(Tests, !DuplicatePresentationFailureOptions &&
			DuplicatePresentationFailureOptions.error().Code == EditorCommandLineErrorCode::DuplicateOption);
		const std::array ConflictingPresentationFailureFixture = {
			std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-fixture"),
			std::string_view("--document-host-test-presentation-failure"),
			std::string_view("--document-host-reset-layout-test") };
		const auto ConflictingPresentationFailureOptions = ParseEditorCommandLine(ConflictingPresentationFailureFixture, Root);
		PF_CHECK(Tests, !ConflictingPresentationFailureOptions &&
			ConflictingPresentationFailureOptions.error().Code == EditorCommandLineErrorCode::UnexpectedArgument);
		const std::array AcquireOutOfDateFixtureArguments = {
			std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-fixture"),
			std::string_view("--document-host-test-acquire-out-of-date") };
		const auto AcquireOutOfDateFixtureOptions = ParseEditorCommandLine(AcquireOutOfDateFixtureArguments, Root);
		PF_CHECK(Tests, AcquireOutOfDateFixtureOptions &&
			AcquireOutOfDateFixtureOptions->EnableDocumentHostAcquireOutOfDateTest);
		const std::array AcquireOutOfDateWithoutFixture = {
			std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-acquire-out-of-date") };
		const auto AcquireOutOfDateWithoutFixtureOptions = ParseEditorCommandLine(AcquireOutOfDateWithoutFixture, Root);
		PF_CHECK(Tests, !AcquireOutOfDateWithoutFixtureOptions &&
			AcquireOutOfDateWithoutFixtureOptions.error().Code == EditorCommandLineErrorCode::UnexpectedArgument);
		const std::array DuplicateAcquireOutOfDateFixture = {
			std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-fixture"),
			std::string_view("--document-host-test-acquire-out-of-date"),
			std::string_view("--document-host-test-acquire-out-of-date") };
		const auto DuplicateAcquireOutOfDateFixtureOptions =
			ParseEditorCommandLine(DuplicateAcquireOutOfDateFixture, Root);
		PF_CHECK(Tests, !DuplicateAcquireOutOfDateFixtureOptions &&
			DuplicateAcquireOutOfDateFixtureOptions.error().Code == EditorCommandLineErrorCode::DuplicateOption);
		const std::array ImageAllocationFailureFixtureArguments = {
			std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-fixture"),
			std::string_view("--document-host-test-image-allocation-failure") };
		const auto ImageAllocationFailureFixtureOptions =
			ParseEditorCommandLine(ImageAllocationFailureFixtureArguments, Root);
		PF_CHECK(Tests, ImageAllocationFailureFixtureOptions &&
			ImageAllocationFailureFixtureOptions->EnableDocumentHostImageAllocationFailureTest);
		const std::array ImageAllocationFailureWithoutFixture = {
			std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-image-allocation-failure") };
		const auto ImageAllocationFailureWithoutFixtureOptions =
			ParseEditorCommandLine(ImageAllocationFailureWithoutFixture, Root);
		PF_CHECK(Tests, !ImageAllocationFailureWithoutFixtureOptions &&
			ImageAllocationFailureWithoutFixtureOptions.error().Code == EditorCommandLineErrorCode::UnexpectedArgument);
		const std::array ConflictingPresentationFailureFixtures = {
			std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-fixture"),
			std::string_view("--document-host-test-acquire-out-of-date"),
			std::string_view("--document-host-test-image-allocation-failure") };
		const auto ConflictingAllocationFailureOptions =
			ParseEditorCommandLine(ConflictingPresentationFailureFixtures, Root);
		PF_CHECK(Tests, !ConflictingAllocationFailureOptions &&
			ConflictingAllocationFailureOptions.error().Code == EditorCommandLineErrorCode::UnexpectedArgument);

		const std::filesystem::path StressManifest = Root / "stress.manifest";
		{
			std::ofstream Manifest(StressManifest, std::ios::binary | std::ios::trunc);
			Manifest << "fixture|fixture.png\n";
			PF_CHECK(Tests, static_cast<bool>(Manifest));
		}
		const std::string StressManifestPath = StressManifest.string();
		const std::array StressArguments = {
			std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--asset-preview-stress"), std::string_view(StressManifestPath), std::string_view("entries") };
		const auto StressOptions = ParseEditorCommandLine(StressArguments, Root);
		PF_CHECK(Tests, StressOptions.has_value());
		PF_CHECK(Tests, StressOptions && StressOptions->AssetPreviewStressManifest == StressManifest);
		PF_CHECK(Tests, StressOptions && StressOptions->AssetPreviewStressBudget == "entries");
		const std::array MissingStressMode = {
			std::string_view("--asset-preview-stress"), std::string_view(StressManifestPath) };
		const auto MissingStressModeResult = ParseEditorCommandLine(MissingStressMode, Root);
		PF_CHECK(Tests, !MissingStressModeResult);

		const std::array MissingValue = { std::string_view("--project") };
		const auto MissingValueResult = ParseEditorCommandLine(MissingValue, Root);
		PF_CHECK(Tests, !MissingValueResult);
		PF_CHECK(Tests, !MissingValueResult &&
			MissingValueResult.error().Code == EditorCommandLineErrorCode::MissingProjectPath);

		const std::array Unexpected = { std::string_view("--unknown") };
		const auto UnexpectedResult = ParseEditorCommandLine(Unexpected, Root);
		PF_CHECK(Tests, !UnexpectedResult);
		PF_CHECK(Tests, !UnexpectedResult &&
			UnexpectedResult.error().Code == EditorCommandLineErrorCode::UnexpectedArgument);

		const std::array Duplicate = {
			std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--project"), std::string_view(AbsoluteProjectPath) };
		const auto DuplicateResult = ParseEditorCommandLine(Duplicate, Root);
		PF_CHECK(Tests, !DuplicateResult);
		PF_CHECK(Tests, !DuplicateResult &&
			DuplicateResult.error().Code == EditorCommandLineErrorCode::DuplicateProjectOption);

		const std::string MissingProjectPath = (Root / "Missing.pfproj").string();
		const std::array InvalidProject = { std::string_view("--project"), std::string_view(MissingProjectPath) };
		const auto InvalidProjectResult = ParseEditorCommandLine(InvalidProject, Root);
		PF_CHECK(Tests, !InvalidProjectResult);
		PF_CHECK(Tests, !InvalidProjectResult &&
			InvalidProjectResult.error().Code == EditorCommandLineErrorCode::InvalidProjectPath);

		auto Loaded = OpenEditorProject(ProjectFile);
		PF_CHECK(Tests, Loaded.has_value());
		PF_CHECK(Tests, Loaded && Loaded->Project.GetDescription().Name == "CLI Project With Spaces");
		PF_CHECK(Tests, Loaded && !Loaded->StartupScene && !Loaded->StartupSceneAsset);

		const std::filesystem::path BrokenProjectFile = Root / "Broken Project.pfproj";
		{
			std::ofstream BrokenProject(BrokenProjectFile, std::ios::binary | std::ios::trunc);
			BrokenProject << "{broken";
			PF_CHECK(Tests, static_cast<bool>(BrokenProject));
		}
		const std::string BrokenProjectPath = BrokenProjectFile.string();
		const std::array BrokenArguments = {
			std::string_view("--project"), std::string_view(BrokenProjectPath) };
		const auto BrokenOptions = ParseEditorCommandLine(BrokenArguments, Root);
		PF_CHECK(Tests, BrokenOptions.has_value());
		if (BrokenOptions)
		{
			const auto BrokenLoad = OpenEditorProject(*BrokenOptions->ProjectFile);
			PF_CHECK(Tests, !BrokenLoad);
			PF_CHECK(Tests, !BrokenLoad && BrokenLoad.error().Message.find("could not be opened") != std::string::npos);
		}
	}

	void TestBundledValidationProjectAssetRegistry(TestRunner& Tests)
	{
		using namespace PulseForge;
		const std::filesystem::path RepositoryRoot = std::filesystem::path(__FILE__).parent_path().parent_path();
		const std::filesystem::path ProjectFile = RepositoryRoot / "PulseForgeGame" / "PulseForgeGame.pfproj";
		auto OpenedProject = Project::Open(ProjectFile);
		PF_CHECK(Tests, OpenedProject.has_value());
		if (!OpenedProject)
			return;

		const std::filesystem::path AssetsRoot = ProjectFile.parent_path() / "Assets";
		const std::vector<AssetRecord> RegisteredAssets = OpenedProject->GetAssetRegistry().GetAssets();
		size_t ManagedSourceCount = 0;
		std::error_code FileError;
		for (std::filesystem::recursive_directory_iterator Iterator(AssetsRoot, FileError), End;
			!FileError && Iterator != End; Iterator.increment(FileError))
		{
			if (!Iterator->is_regular_file(FileError) || FileError || Iterator->path().extension() == ".meta")
				continue;

			++ManagedSourceCount;
			const std::filesystem::path RelativePath = Iterator->path().lexically_relative(ProjectFile.parent_path());
			const auto Record = std::find_if(RegisteredAssets.begin(), RegisteredAssets.end(),
				[&RelativePath](const AssetRecord& Asset) { return Asset.ProjectRelativePath == RelativePath; });
			PF_CHECK(Tests, Record != RegisteredAssets.end());
		}
		PF_CHECK(Tests, !FileError);
		PF_CHECK(Tests, RegisteredAssets.size() == ManagedSourceCount);

		const std::filesystem::path DemoProjectFile = RepositoryRoot / "Samples" / "VisualValidation" /
			"PointLightShadows" / "PointLightShadows.pfproj";
		auto DemoProject = Project::Open(DemoProjectFile);
		PF_CHECK(Tests, DemoProject.has_value());
		if (!DemoProject)
			return;
		PF_CHECK(Tests, DemoProject->GetDescription().StartScene.has_value());
		if (!DemoProject->GetDescription().StartScene)
			return;
		const auto StartupRecord = DemoProject->GetAssetRegistry().Find(*DemoProject->GetDescription().StartScene);
		PF_CHECK(Tests, StartupRecord.has_value());
		PF_CHECK(Tests, StartupRecord && StartupRecord->ProjectRelativePath.filename() == "PointLightShadowsOverview.scene");
		size_t LoadedDemoSceneCount = 0;
		for (const AssetRecord& Asset : DemoProject->GetAssetRegistry().GetAssets())
		{
			if (Asset.ProjectRelativePath.extension() != ".scene")
				continue;

			Scene LoadedDemoScene;
			PF_CHECK(Tests, SceneAssetService::Load(Asset.ID, DemoProject->GetRootPath(),
				DemoProject->GetAssetRegistry(), LoadedDemoScene).has_value());
			const auto ReferenceIssues = AssetReferenceValidator::Validate(
				LoadedDemoScene, DemoProject->GetAssetRegistry());
			PF_CHECK(Tests, ReferenceIssues.has_value());
			PF_CHECK(Tests, ReferenceIssues && ReferenceIssues->empty());
			++LoadedDemoSceneCount;
		}
		PF_CHECK(Tests, LoadedDemoSceneCount == 4);

		const std::filesystem::path EditorWorkflowProjectFile = RepositoryRoot / "Samples" / "VisualValidation" /
			"EditorWorkflow" / "EditorWorkflow.pfproj";
		auto EditorWorkflowProject = Project::Open(EditorWorkflowProjectFile);
		PF_CHECK(Tests, EditorWorkflowProject.has_value());
		if (!EditorWorkflowProject)
			return;
		PF_CHECK(Tests, EditorWorkflowProject->GetDescription().StartScene.has_value());
		if (!EditorWorkflowProject->GetDescription().StartScene)
			return;
		const auto EditorWorkflowStartup = EditorWorkflowProject->GetAssetRegistry().Find(
			*EditorWorkflowProject->GetDescription().StartScene);
		PF_CHECK(Tests, EditorWorkflowStartup.has_value());
		PF_CHECK(Tests, EditorWorkflowStartup &&
			EditorWorkflowStartup->ProjectRelativePath.filename() == "EditorWorkflowOverview.scene");
		Scene EditorWorkflowScene;
		PF_CHECK(Tests, SceneAssetService::Load(*EditorWorkflowProject->GetDescription().StartScene,
			EditorWorkflowProject->GetRootPath(), EditorWorkflowProject->GetAssetRegistry(), EditorWorkflowScene).has_value());
		const auto EditorWorkflowReferences = AssetReferenceValidator::Validate(
			EditorWorkflowScene, EditorWorkflowProject->GetAssetRegistry());
		PF_CHECK(Tests, EditorWorkflowReferences.has_value());
		PF_CHECK(Tests, EditorWorkflowReferences && EditorWorkflowReferences->empty());
		PF_CHECK(Tests, EditorWorkflowScene.GetEntityCount() == 13);
		const std::vector<AssetRecord> WorkflowAssets = EditorWorkflowProject->GetAssetRegistry().GetAssets();
		size_t WorkflowMaterialCount = 0;
		for (const AssetRecord& Asset : WorkflowAssets)
			if (Asset.ProjectRelativePath.extension() == ".material")
			{
				const auto Material = MaterialAssetService::Load(
					Asset.ID, EditorWorkflowProject->GetRootPath(), EditorWorkflowProject->GetAssetRegistry());
				PF_CHECK(Tests, Material.has_value());
				++WorkflowMaterialCount;
			}
		PF_CHECK(Tests, WorkflowMaterialCount == 4);
		const auto WorkflowMesh = std::find_if(WorkflowAssets.begin(), WorkflowAssets.end(), [](const AssetRecord& Asset)
			{
				return Asset.ProjectRelativePath == std::filesystem::path("Assets/Models/WorkflowCube.gltf");
			});
		PF_CHECK(Tests, WorkflowMesh != WorkflowAssets.end());
		if (WorkflowMesh != WorkflowAssets.end())
		{
			const auto ImportedWorkflowMesh = GltfMeshImporter::ImportStaticPrimitive(
				WorkflowMesh->ID, EditorWorkflowProject->GetRootPath(), EditorWorkflowProject->GetAssetRegistry());
			PF_CHECK(Tests, ImportedWorkflowMesh.has_value());
			PF_CHECK(Tests, ImportedWorkflowMesh && !ImportedWorkflowMesh->Vertices.empty() &&
				!ImportedWorkflowMesh->Indices.empty());
		}
	}

	void TestAssetReferenceValidation(TestRunner& Tests)
	{
		using namespace PulseForge;
		std::error_code FileError;
		const std::filesystem::path TemporaryDirectory = std::filesystem::temp_directory_path(FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError)
			return;

		const auto ProjectIdentifier = UUID::Generate();
		const auto MissingAssetIdentifier = UUID::Generate();
		PF_CHECK(Tests, ProjectIdentifier && MissingAssetIdentifier);
		if (!ProjectIdentifier || !MissingAssetIdentifier)
			return;

		const std::filesystem::path ProjectRoot =
			TemporaryDirectory / ("PulseForgeAssetReferences-" + ProjectIdentifier->ToString());
		struct ProjectDirectoryCleanup
		{
			std::filesystem::path Path;
			~ProjectDirectoryCleanup()
			{
				std::error_code Error;
				std::filesystem::remove_all(Path, Error);
			}
		} Cleanup{ ProjectRoot };

		const auto CreateManagedAsset = [&](std::string_view RelativePath, std::string_view Contents)
			-> std::optional<AssetMetadata>
		{
			const std::filesystem::path Path = ProjectRoot / RelativePath;
			std::filesystem::create_directories(Path.parent_path(), FileError);
			PF_CHECK(Tests, !FileError);
			if (FileError)
				return std::nullopt;
			{
				std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
				Output.write(Contents.data(), static_cast<std::streamsize>(Contents.size()));
				PF_CHECK(Tests, static_cast<bool>(Output));
				if (!Output)
					return std::nullopt;
			}
			auto Metadata = AssetMetadataSerializer::CreateForNewAsset(Path);
			PF_CHECK(Tests, Metadata.has_value());
			return Metadata ? std::optional<AssetMetadata>{ *Metadata } : std::nullopt;
		};

		const auto MeshMetadata = CreateManagedAsset("Assets/Meshes/mesh.Gltf", "mesh source");
		const auto MaterialMetadata = CreateManagedAsset("Assets/Materials/surface.material", "material");
		const auto WrongTypeMetadata = CreateManagedAsset("Assets/Misc/not-an-asset.txt", "other data");
		const auto ScriptMetadata = CreateManagedAsset("Assets/Scripts/controller.lua", "return {}\n");
		const auto AudioMetadata = CreateManagedAsset("Assets/Audio/sound.WAV", "audio data");
		const auto HdrMetadata = CreateManagedAsset("Assets/Environments/studio.hdr", "hdr data");
		PF_CHECK(Tests, MeshMetadata && MaterialMetadata && WrongTypeMetadata && ScriptMetadata && AudioMetadata && HdrMetadata);
		if (!MeshMetadata || !MaterialMetadata || !WrongTypeMetadata || !ScriptMetadata || !AudioMetadata || !HdrMetadata)
			return;

		Scene TestScene;
		const auto ValidScriptOnly = TestScene.CreateEntity("Valid script only");
		const auto MissingScriptOnly = TestScene.CreateEntity("Missing script only");
		const auto WrongScriptOnly = TestScene.CreateEntity("Wrong script only");
		const auto ValidAudioOnly = TestScene.CreateEntity("Valid audio only");
		const auto MissingAudioOnly = TestScene.CreateEntity("Missing audio only");
		const auto WrongAudioOnly = TestScene.CreateEntity("Wrong audio only");
		const auto WrongMeshType = TestScene.CreateEntity("Wrong mesh type");
		const auto WrongMaterialType = TestScene.CreateEntity("Wrong material type");
		const auto ValidMeshAndMaterial = TestScene.CreateEntity("Valid mesh and material");
		const auto MultipleReferences = TestScene.CreateEntity("Multiple references");
		const auto ValidEnvironment = TestScene.CreateEntity("Valid environment");
		const auto WrongEnvironment = TestScene.CreateEntity("Wrong environment");
		const auto Unrelated = TestScene.CreateEntity("Unrelated");
		PF_CHECK(Tests, ValidScriptOnly && MissingScriptOnly && WrongScriptOnly && ValidAudioOnly && MissingAudioOnly &&
			WrongAudioOnly && WrongMeshType && WrongMaterialType && ValidMeshAndMaterial && MultipleReferences &&
			ValidEnvironment && WrongEnvironment && Unrelated);
		if (!ValidScriptOnly || !MissingScriptOnly || !WrongScriptOnly || !ValidAudioOnly || !MissingAudioOnly ||
			!WrongAudioOnly || !WrongMeshType || !WrongMaterialType || !ValidMeshAndMaterial || !MultipleReferences ||
			!ValidEnvironment || !WrongEnvironment || !Unrelated)
			return;

		PF_CHECK(Tests, ValidScriptOnly->SetScript(ScriptComponent{ ScriptMetadata->ID }).has_value());
		PF_CHECK(Tests, MissingScriptOnly->SetScript(ScriptComponent{ *MissingAssetIdentifier, false }).has_value());
		PF_CHECK(Tests, WrongScriptOnly->SetScript(ScriptComponent{ WrongTypeMetadata->ID }).has_value());
		PF_CHECK(Tests, ValidAudioOnly->SetAudioSource(AudioSourceComponent{ AudioMetadata->ID }).has_value());
		PF_CHECK(Tests, MissingAudioOnly->SetAudioSource(AudioSourceComponent{ *MissingAssetIdentifier }).has_value());
		PF_CHECK(Tests, WrongAudioOnly->SetAudioSource(AudioSourceComponent{ WrongTypeMetadata->ID }).has_value());
		PF_CHECK(Tests, WrongMeshType->SetMeshRenderer(MeshRendererComponent{ WrongTypeMetadata->ID }).has_value());
		PF_CHECK(Tests, WrongMaterialType->SetMeshRenderer(
			MeshRendererComponent{ MeshMetadata->ID, WrongTypeMetadata->ID }).has_value());
		PF_CHECK(Tests, ValidMeshAndMaterial->SetMeshRenderer(
			MeshRendererComponent{ MeshMetadata->ID, MaterialMetadata->ID }).has_value());
		PF_CHECK(Tests, MultipleReferences->SetMeshRenderer(
			MeshRendererComponent{ *MissingAssetIdentifier, *MissingAssetIdentifier }).has_value());
		PF_CHECK(Tests, MultipleReferences->SetScript(ScriptComponent{ *MissingAssetIdentifier }).has_value());
		PF_CHECK(Tests, MultipleReferences->SetAudioSource(AudioSourceComponent{ *MissingAssetIdentifier }).has_value());
		PF_CHECK(Tests, ValidEnvironment->SetEnvironmentLight(EnvironmentLightComponent{ HdrMetadata->ID, 0.8f }).has_value());
		PF_CHECK(Tests, WrongEnvironment->SetEnvironmentLight(EnvironmentLightComponent{ WrongTypeMetadata->ID, 1.0f }).has_value());

		const auto HasIssue = [](const std::vector<AssetReferenceIssue>& Issues,
			const Entity& Owner,
			AssetReferenceKind Kind,
			AssetReferenceIssueCode Code,
			AssetID Asset)
		{
			return std::ranges::any_of(Issues, [&](const AssetReferenceIssue& Issue)
			{
				return Issue.Entity == Owner.GetUUID() && Issue.Kind == Kind && Issue.Code == Code && Issue.Asset == Asset;
			});
		};

		AssetRegistry EmptyRegistry;
		const auto AllMissing = AssetReferenceValidator::Validate(TestScene, EmptyRegistry);
		PF_CHECK(Tests, AllMissing && AllMissing->size() == 17);
		PF_CHECK(Tests, AllMissing && HasIssue(*AllMissing, *ValidScriptOnly, AssetReferenceKind::Script,
			AssetReferenceIssueCode::MissingAsset, ScriptMetadata->ID));
		PF_CHECK(Tests, AllMissing && HasIssue(*AllMissing, *ValidAudioOnly, AssetReferenceKind::Audio,
			AssetReferenceIssueCode::MissingAsset, AudioMetadata->ID));

		AssetRegistry Registry;
		PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());
		const auto Validation = AssetReferenceValidator::Validate(TestScene, Registry);
		PF_CHECK(Tests, Validation && Validation->size() == 11);
		if (!Validation)
			return;

		PF_CHECK(Tests, HasIssue(*Validation, *MissingScriptOnly, AssetReferenceKind::Script,
			AssetReferenceIssueCode::MissingAsset, *MissingAssetIdentifier));
		PF_CHECK(Tests, HasIssue(*Validation, *WrongScriptOnly, AssetReferenceKind::Script,
			AssetReferenceIssueCode::WrongAssetType, WrongTypeMetadata->ID));
		PF_CHECK(Tests, HasIssue(*Validation, *MissingAudioOnly, AssetReferenceKind::Audio,
			AssetReferenceIssueCode::MissingAsset, *MissingAssetIdentifier));
		PF_CHECK(Tests, HasIssue(*Validation, *WrongAudioOnly, AssetReferenceKind::Audio,
			AssetReferenceIssueCode::WrongAssetType, WrongTypeMetadata->ID));
		PF_CHECK(Tests, HasIssue(*Validation, *WrongEnvironment, AssetReferenceKind::Environment,
			AssetReferenceIssueCode::WrongAssetType, WrongTypeMetadata->ID));
		PF_CHECK(Tests, !HasIssue(*Validation, *ValidEnvironment, AssetReferenceKind::Environment,
			AssetReferenceIssueCode::WrongAssetType, HdrMetadata->ID));
		PF_CHECK(Tests, HasIssue(*Validation, *WrongMeshType, AssetReferenceKind::Mesh,
			AssetReferenceIssueCode::WrongAssetType, WrongTypeMetadata->ID));
		PF_CHECK(Tests, HasIssue(*Validation, *WrongMaterialType, AssetReferenceKind::Material,
			AssetReferenceIssueCode::WrongAssetType, WrongTypeMetadata->ID));
		PF_CHECK(Tests, !HasIssue(*Validation, *ValidScriptOnly, AssetReferenceKind::Script,
			AssetReferenceIssueCode::WrongAssetType, ScriptMetadata->ID));
		PF_CHECK(Tests, !HasIssue(*Validation, *ValidAudioOnly, AssetReferenceKind::Audio,
			AssetReferenceIssueCode::WrongAssetType, AudioMetadata->ID));
		PF_CHECK(Tests, !HasIssue(*Validation, *ValidMeshAndMaterial, AssetReferenceKind::Mesh,
			AssetReferenceIssueCode::WrongAssetType, MeshMetadata->ID));
		PF_CHECK(Tests, !HasIssue(*Validation, *ValidMeshAndMaterial, AssetReferenceKind::Material,
			AssetReferenceIssueCode::WrongAssetType, MaterialMetadata->ID));
		PF_CHECK(Tests, HasIssue(*Validation, *MultipleReferences, AssetReferenceKind::Mesh,
			AssetReferenceIssueCode::MissingAsset, *MissingAssetIdentifier));
		PF_CHECK(Tests, HasIssue(*Validation, *MultipleReferences, AssetReferenceKind::Material,
			AssetReferenceIssueCode::MissingAsset, *MissingAssetIdentifier));
		PF_CHECK(Tests, HasIssue(*Validation, *MultipleReferences, AssetReferenceKind::Script,
			AssetReferenceIssueCode::MissingAsset, *MissingAssetIdentifier));
		PF_CHECK(Tests, HasIssue(*Validation, *MultipleReferences, AssetReferenceKind::Audio,
			AssetReferenceIssueCode::MissingAsset, *MissingAssetIdentifier));
		PF_CHECK(Tests, std::ranges::none_of(*Validation, [&](const AssetReferenceIssue& Issue)
		{
			return Issue.Entity == Unrelated->GetUUID();
		}));
	}

	void TestScriptRuntime(TestRunner& Tests)
	{
		using namespace PulseForge;
		std::error_code FileError;
		const std::filesystem::path TemporaryDirectory = std::filesystem::temp_directory_path(FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError)
			return;

		const auto ProjectIdentifier = UUID::Generate();
		PF_CHECK(Tests, ProjectIdentifier.has_value());
		if (!ProjectIdentifier)
			return;

		const std::filesystem::path ProjectRoot = TemporaryDirectory / ("PulseForgeScriptRuntime-" + ProjectIdentifier->ToString());
		struct ProjectCleanup
		{
			std::filesystem::path Path;
			~ProjectCleanup()
			{
				std::error_code Error;
				std::filesystem::remove_all(Path, Error);
			}
		} Cleanup{ ProjectRoot };

		auto ProjectResult = Project::Create(ProjectRoot / "ScriptProject.pfproj", "Script Runtime Tests");
		PF_CHECK(Tests, ProjectResult.has_value());
		if (!ProjectResult)
			return;
		std::filesystem::create_directories(ProjectRoot / "Assets/Scripts", FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError)
			return;

		const auto CreateScriptAsset = [&](std::string_view FileName, std::string_view Source)
		{
			const auto Contents = std::as_bytes(std::span(Source.data(), Source.size()));
			return AssetOperations::CreateAssetFromBytes(
				ProjectResult->GetAssetRegistry(),
				ProjectResult->GetRootPath(),
				Contents,
				std::filesystem::path("Assets/Scripts") / FileName);
		};

		constexpr std::string_view TransformScript = R"(
local elapsed = 0
function OnCreate()
  assert(io == nil and os == nil and debug == nil and package == nil)
  assert(load == nil and loadfile == nil and dofile == nil and collectgarbage == nil)
  assert(pcall == nil and xpcall == nil)
  assert(string.dump == nil)
  local found = scene.find_entity(entity:uuid())
  assert(found ~= nil and found:uuid() == entity:uuid())
end
function OnUpdate(dt)
  elapsed = elapsed + dt
  assert(entity:set_translation(elapsed, 2, 3))
end
function OnDestroy()
  assert(entity:set_translation(-1, 2, 3))
end
)";
		const auto TransformScriptAsset = CreateScriptAsset("transform.lua", TransformScript);
		PF_CHECK(Tests, TransformScriptAsset.has_value());
		if (!TransformScriptAsset)
			return;

		const auto RelativeMotionScriptAsset = CreateScriptAsset(
			"relative_motion.lua",
			"local elapsed = 0\n"
			"local initialX, initialY, initialZ = 0, 0, 0\n"
			"function OnCreate()\n"
			"  initialX, initialY, initialZ = entity:get_translation()\n"
			"  assert(initialX ~= nil and initialY ~= nil and initialZ ~= nil)\n"
			"end\n"
			"function OnUpdate(dt)\n"
			"  elapsed = elapsed + dt\n"
			"  assert(entity:set_translation(initialX + math.sin(elapsed) * 0.25, initialY, initialZ))\n"
			"end\n");
		PF_CHECK(Tests, RelativeMotionScriptAsset.has_value());
		if (!RelativeMotionScriptAsset)
			return;

		Scene RelativeMotionScene;
		const auto OriginalMover = RelativeMotionScene.CreateEntity("Original mover");
		PF_CHECK(Tests, OriginalMover.has_value());
		if (!OriginalMover)
			return;
		TransformComponent OriginalTransform;
		OriginalTransform.Translation = { 3.0f, 2.0f, -4.0f };
		PF_CHECK(Tests, OriginalMover->SetTransform(OriginalTransform).has_value());
		PF_CHECK(Tests, OriginalMover->SetScript(ScriptComponent{ RelativeMotionScriptAsset->ID }).has_value());
		const auto DuplicatedMover = RelativeMotionScene.DuplicateEntity(*OriginalMover);
		PF_CHECK(Tests, DuplicatedMover.has_value());
		if (!DuplicatedMover)
			return;
		TransformComponent DuplicatedTransform;
		DuplicatedTransform.Translation = { -7.0f, 5.0f, 11.0f };
		PF_CHECK(Tests, DuplicatedMover->SetTransform(DuplicatedTransform).has_value());
		ScriptRuntime RelativeMotionRuntime(*ProjectResult);
		PF_CHECK(Tests, RelativeMotionRuntime.Start(RelativeMotionScene).has_value());
		PF_CHECK(Tests, RelativeMotionRuntime.Advance(RelativeMotionScene, Timestep(0.5)).has_value());
		const auto OriginalMovedTransform = OriginalMover->GetTransform();
		const auto DuplicatedMovedTransform = DuplicatedMover->GetTransform();
		const float ExpectedOscillation = std::sin(0.5f) * 0.25f;
		PF_CHECK(Tests, OriginalMovedTransform &&
			std::abs(OriginalMovedTransform->Translation.x - (3.0f + ExpectedOscillation)) < 0.0001f &&
			std::abs(OriginalMovedTransform->Translation.y - 2.0f) < 0.0001f &&
			std::abs(OriginalMovedTransform->Translation.z + 4.0f) < 0.0001f);
		PF_CHECK(Tests, DuplicatedMovedTransform &&
			std::abs(DuplicatedMovedTransform->Translation.x - (-7.0f + ExpectedOscillation)) < 0.0001f &&
			std::abs(DuplicatedMovedTransform->Translation.y - 5.0f) < 0.0001f &&
			std::abs(DuplicatedMovedTransform->Translation.z - 11.0f) < 0.0001f);
		PF_CHECK(Tests, RelativeMotionRuntime.GetDiagnostics().empty());
		RelativeMotionRuntime.Stop();

		std::filesystem::create_directories(ProjectRoot / "Assets/Prefabs", FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError)
			return;
		const auto PrefabChildScriptAsset = CreateScriptAsset(
			"prefab_child.lua",
			"function OnCreate() assert(entity:set_translation(9, 8, 7)) end\n");
		PF_CHECK(Tests, PrefabChildScriptAsset.has_value());
		if (!PrefabChildScriptAsset)
			return;

		Scene ScriptSpawnPrefabSource;
		const auto ScriptSpawnPrefabRoot = ScriptSpawnPrefabSource.CreateEntity("Script-spawned prefab root");
		const auto ScriptSpawnPrefabChild = ScriptSpawnPrefabSource.CreateEntity("Script-spawned prefab child");
		PF_CHECK(Tests, ScriptSpawnPrefabRoot && ScriptSpawnPrefabChild);
		if (!ScriptSpawnPrefabRoot || !ScriptSpawnPrefabChild)
			return;
		PF_CHECK(Tests, ScriptSpawnPrefabChild->SetParent(*ScriptSpawnPrefabRoot).has_value());
		PF_CHECK(Tests, ScriptSpawnPrefabChild->SetScript(ScriptComponent{ PrefabChildScriptAsset->ID }).has_value());
		const auto ScriptSpawnPrefab = PrefabAssetService::Create(
			ProjectResult->GetAssetRegistry(),
			ProjectResult->GetRootPath(),
			"Assets/Prefabs/ScriptSpawn.prefab",
			ScriptSpawnPrefabSource,
			*ScriptSpawnPrefabRoot);
		PF_CHECK(Tests, ScriptSpawnPrefab.has_value());
		if (!ScriptSpawnPrefab)
			return;

		const std::string SceneMutationScript =
			"local CreatedEntity\n"
			"local SpawnedPrefab\n"
			"local Destroyed = false\n"
			"function OnCreate()\n"
			"  local invalidName, nameMessage = scene.create_entity(17)\n"
			"  assert(invalidName == nil and nameMessage ~= nil)\n"
			"  CreatedEntity, nameMessage = scene.create_entity('Lua-created entity')\n"
			"  assert(CreatedEntity, nameMessage)\n"
			"  local found = scene.find_entity(CreatedEntity:uuid())\n"
			"  assert(found ~= nil and found:uuid() == CreatedEntity:uuid())\n"
			"  local invalidPrefab, prefabMessage = scene.spawn_prefab('not-a-uuid')\n"
			"  assert(invalidPrefab == nil and prefabMessage ~= nil)\n"
			"  local wrongType = scene.spawn_prefab('" + TransformScriptAsset->ID.ToString() + "')\n"
			"  assert(wrongType == nil)\n"
			"  SpawnedPrefab, prefabMessage = scene.spawn_prefab('" + ScriptSpawnPrefab->ID.ToString() + "')\n"
			"  assert(SpawnedPrefab, prefabMessage)\n"
			"end\n"
			"function OnUpdate()\n"
			"  if not Destroyed then\n"
			"    assert(CreatedEntity:destroy())\n"
			"    Destroyed = true\n"
			"    assert(scene.find_entity(CreatedEntity:uuid()) == nil)\n"
			"    local x, staleMessage = CreatedEntity:get_translation()\n"
			"    assert(x == nil and staleMessage ~= nil)\n"
			"    local transient, message = scene.create_entity('Transient Lua entity')\n"
			"    assert(transient, message)\n"
			"    assert(transient:destroy())\n"
			"    assert(entity:destroy())\n"
			"  end\n"
			"  assert(SpawnedPrefab:set_translation(4, 5, 6))\n"
			"end\n"
			"function OnDestroy()\n"
			"  local x, staleMessage = entity:get_translation()\n"
			"  assert(x == nil and staleMessage ~= nil)\n"
			"end\n";
		const auto SceneMutationScriptAsset = CreateScriptAsset("scene_mutation.lua", SceneMutationScript);
		PF_CHECK(Tests, SceneMutationScriptAsset.has_value());
		if (!SceneMutationScriptAsset)
			return;

		Scene SceneMutationTestScene;
		const auto SceneMutationController = SceneMutationTestScene.CreateEntity("Scene mutation controller");
		PF_CHECK(Tests, SceneMutationController.has_value());
		if (!SceneMutationController)
			return;
		PF_CHECK(Tests, SceneMutationController->SetScript(ScriptComponent{ SceneMutationScriptAsset->ID }).has_value());
		ScriptRuntime SceneMutationRuntime(*ProjectResult);
		PF_CHECK(Tests, SceneMutationRuntime.Start(SceneMutationTestScene).has_value());
		PF_CHECK(Tests, SceneMutationRuntime.GetDiagnostics().empty());
		PF_CHECK(Tests, SceneMutationTestScene.GetEntityCount() == 4);

		std::optional<Entity> SpawnedPrefabRoot;
		std::optional<Entity> SpawnedPrefabChild;
		for (const Entity& Current : SceneMutationTestScene.GetEntities())
		{
			const auto Tag = Current.GetTag();
			if (Tag && Tag->Name == "Script-spawned prefab root")
				SpawnedPrefabRoot = Current;
			else if (Tag && Tag->Name == "Script-spawned prefab child")
				SpawnedPrefabChild = Current;
		}
		PF_CHECK(Tests, SpawnedPrefabRoot.has_value() && SpawnedPrefabChild.has_value());
		if (SpawnedPrefabRoot && SpawnedPrefabChild)
		{
			const auto SpawnedChildParent = SpawnedPrefabChild->GetParent();
			PF_CHECK(Tests, SpawnedChildParent && SpawnedChildParent->has_value() &&
				(**SpawnedChildParent).GetUUID() == SpawnedPrefabRoot->GetUUID());
		}

		PF_CHECK(Tests, SceneMutationRuntime.Advance(SceneMutationTestScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, SceneMutationTestScene.GetEntityCount() == 2);
		if (SpawnedPrefabRoot)
		{
			const auto SpawnedTransform = SpawnedPrefabRoot->GetTransform();
			PF_CHECK(Tests, SpawnedTransform &&
				std::abs(SpawnedTransform->Translation.x - 4.0f) < 0.0001f &&
				std::abs(SpawnedTransform->Translation.y - 5.0f) < 0.0001f &&
				std::abs(SpawnedTransform->Translation.z - 6.0f) < 0.0001f);
		}
		PF_CHECK(Tests, SceneMutationRuntime.GetDiagnostics().empty());
		PF_CHECK(Tests, SceneMutationRuntime.GetScriptCount() == 2);
		if (SpawnedPrefabChild)
		{
			const auto SpawnedChildTransform = SpawnedPrefabChild->GetTransform();
			PF_CHECK(Tests, SpawnedChildTransform &&
				std::abs(SpawnedChildTransform->Translation.x - 9.0f) < 0.0001f &&
				std::abs(SpawnedChildTransform->Translation.y - 8.0f) < 0.0001f &&
				std::abs(SpawnedChildTransform->Translation.z - 7.0f) < 0.0001f);
		}
		PF_CHECK(Tests, SceneMutationRuntime.Advance(SceneMutationTestScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, SceneMutationRuntime.GetScriptCount() == 1);
		PF_CHECK(Tests, SceneMutationRuntime.GetDiagnostics().empty());
		SceneMutationRuntime.Stop();

		Scene TestScene;
		const auto FirstEntity = TestScene.CreateEntity("First scripted entity");
		const auto SecondEntity = TestScene.CreateEntity("Second scripted entity");
		const auto DisabledEntity = TestScene.CreateEntity("Disabled scripted entity");
		PF_CHECK(Tests, FirstEntity && SecondEntity && DisabledEntity);
		if (!FirstEntity || !SecondEntity || !DisabledEntity)
			return;
		PF_CHECK(Tests, FirstEntity->SetScript(ScriptComponent{ TransformScriptAsset->ID }).has_value());
		PF_CHECK(Tests, SecondEntity->SetScript(ScriptComponent{ TransformScriptAsset->ID }).has_value());
		PF_CHECK(Tests, DisabledEntity->SetScript(ScriptComponent{ TransformScriptAsset->ID, false }).has_value());

		ScriptRuntime Runtime(*ProjectResult);
		const auto BeforeStart = Runtime.Advance(TestScene, Timestep(0.1));
		PF_CHECK(Tests, !BeforeStart && BeforeStart.error().Code == ScriptRuntimeErrorCode::NotRunning);
		PF_CHECK(Tests, Runtime.Start(TestScene).has_value());
		PF_CHECK(Tests, Runtime.IsRunning() && Runtime.GetScriptCount() == 2);
		PF_CHECK(Tests, Runtime.GetDiagnostics().empty());
		const auto DuplicateStart = Runtime.Start(TestScene);
		PF_CHECK(Tests, !DuplicateStart && DuplicateStart.error().Code == ScriptRuntimeErrorCode::AlreadyRunning);
		Scene OtherScene;
		const auto DifferentScene = Runtime.Advance(OtherScene, Timestep(0.1));
		PF_CHECK(Tests, !DifferentScene && DifferentScene.error().Code == ScriptRuntimeErrorCode::DifferentScene);
		const auto NegativeDelta = Runtime.Advance(TestScene, Timestep(-0.1));
		PF_CHECK(Tests, !NegativeDelta && NegativeDelta.error().Code == ScriptRuntimeErrorCode::InvalidDeltaTime);
		PF_CHECK(Tests, Runtime.Advance(TestScene, Timestep(0.25)).has_value());
		const auto FirstTransform = FirstEntity->GetTransform();
		const auto SecondTransform = SecondEntity->GetTransform();
		PF_CHECK(Tests, FirstTransform && std::abs(FirstTransform->Translation.x - 0.25f) < 0.0001f);
		PF_CHECK(Tests, SecondTransform && std::abs(SecondTransform->Translation.x - 0.25f) < 0.0001f);

		PF_CHECK(Tests, FirstEntity->RemoveScript().has_value());
		PF_CHECK(Tests, Runtime.Advance(TestScene, Timestep(0.1)).has_value());
		PF_CHECK(Tests, Runtime.GetScriptCount() == 1);
		const auto DetachedTransform = FirstEntity->GetTransform();
		const auto ActiveTransform = SecondEntity->GetTransform();
		PF_CHECK(Tests, DetachedTransform && DetachedTransform->Translation.x == -1.0f);
		PF_CHECK(Tests, ActiveTransform && std::abs(ActiveTransform->Translation.x - 0.35f) < 0.0001f);
		Runtime.Stop();
		PF_CHECK(Tests, !Runtime.IsRunning() && Runtime.GetScriptCount() == 0);
		const auto StoppedTransform = SecondEntity->GetTransform();
		PF_CHECK(Tests, StoppedTransform && StoppedTransform->Translation.x == -1.0f);
		Runtime.Stop();

		Scene CoordinatedScene;
		const auto CoordinatedScriptEntity = CoordinatedScene.CreateEntity("Coordinated script");
		const auto CoordinatedPhysicsEntity = CoordinatedScene.CreateEntity("Coordinated body");
		PF_CHECK(Tests, CoordinatedScriptEntity && CoordinatedPhysicsEntity);
		if (!CoordinatedScriptEntity || !CoordinatedPhysicsEntity)
			return;
		PF_CHECK(Tests, CoordinatedScriptEntity->SetScript(ScriptComponent{ TransformScriptAsset->ID }).has_value());
		PF_CHECK(Tests, CoordinatedPhysicsEntity->SetRigidbody(RigidbodyComponent{}).has_value());
		PF_CHECK(Tests, CoordinatedPhysicsEntity->SetBoxCollider(BoxColliderComponent{}).has_value());

		SceneRuntimeDesc RuntimeDescription;
		RuntimeDescription.Audio.OutputBackend = AudioOutputBackend::Null;
		RuntimeDescription.Physics.FixedStepSeconds = 0.1;
		RuntimeDescription.Physics.MaxSubsteps = 2;
		RuntimeDescription.Physics.MaxFrameDeltaSeconds = 0.25;
		SceneRuntime CoordinatedRuntime(*ProjectResult, RuntimeDescription);
		const auto BeforeRuntimeStart = CoordinatedRuntime.Advance(CoordinatedScene, Timestep(0.1));
		PF_CHECK(Tests, !BeforeRuntimeStart && BeforeRuntimeStart.error().Code == SceneRuntimeErrorCode::NotRunning);
		PF_CHECK(Tests, CoordinatedRuntime.Start(CoordinatedScene).has_value());
		PF_CHECK(Tests, CoordinatedRuntime.IsRunning());
		const auto DuplicateRuntimeStart = CoordinatedRuntime.Start(CoordinatedScene);
		PF_CHECK(Tests, !DuplicateRuntimeStart && DuplicateRuntimeStart.error().Code == SceneRuntimeErrorCode::AlreadyRunning);
		const auto CoordinatedNegativeDelta = CoordinatedRuntime.Advance(CoordinatedScene, Timestep(-0.1));
		PF_CHECK(Tests,
			!CoordinatedNegativeDelta && CoordinatedNegativeDelta.error().Code == SceneRuntimeErrorCode::InvalidDeltaTime);
		const auto CoordinatedUpdate = CoordinatedRuntime.Advance(CoordinatedScene, Timestep(0.25));
		PF_CHECK(Tests, CoordinatedUpdate && CoordinatedUpdate->PhysicsSteps == 2);
		const auto CoordinatedTransform = CoordinatedScriptEntity->GetTransform();
		const auto PhysicsTransform = CoordinatedPhysicsEntity->GetTransform();
		PF_CHECK(Tests, CoordinatedTransform && std::abs(CoordinatedTransform->Translation.x - 0.25f) < 0.0001f);
		PF_CHECK(Tests, PhysicsTransform && PhysicsTransform->Translation.y < 0.0f);
		Scene OtherRuntimeScene;
		const auto WrongRuntimeScene = CoordinatedRuntime.Advance(OtherRuntimeScene, Timestep(0.1));
		PF_CHECK(Tests, !WrongRuntimeScene && WrongRuntimeScene.error().Code == SceneRuntimeErrorCode::DifferentScene);
		CoordinatedRuntime.Stop();
		PF_CHECK(Tests, !CoordinatedRuntime.IsRunning());
		const auto CoordinatedStoppedTransform = CoordinatedScriptEntity->GetTransform();
		PF_CHECK(Tests,
			CoordinatedStoppedTransform && CoordinatedStoppedTransform->Translation.x == -1.0f);

		constexpr std::string_view ForceScript = R"(
function OnUpdate()
  local applied, message = entity:apply_force(0, 100, 0)
  assert(applied, message)
end
)";
		const auto ForceScriptAsset = CreateScriptAsset("force.lua", ForceScript);
		PF_CHECK(Tests, ForceScriptAsset.has_value());
		if (!ForceScriptAsset)
			return;
		Scene ScriptedPhysicsScene;
		const auto ScriptedBody = ScriptedPhysicsScene.CreateEntity("Script-driven body");
		PF_CHECK(Tests, ScriptedBody.has_value());
		if (!ScriptedBody)
			return;
		PF_CHECK(Tests, ScriptedBody->SetRigidbody(RigidbodyComponent{}).has_value());
		PF_CHECK(Tests, ScriptedBody->SetBoxCollider(BoxColliderComponent{}).has_value());
		PF_CHECK(Tests, ScriptedBody->SetScript(ScriptComponent{ ForceScriptAsset->ID }).has_value());
		SceneRuntime ScriptedPhysicsRuntime(*ProjectResult, RuntimeDescription);
		PF_CHECK(Tests, ScriptedPhysicsRuntime.Start(ScriptedPhysicsScene).has_value());
		const auto ScriptedPhysicsFrame = ScriptedPhysicsRuntime.Advance(ScriptedPhysicsScene, Timestep(0.1));
		const auto ScriptedBodyTransform = ScriptedBody->GetTransform();
		PF_CHECK(Tests, ScriptedPhysicsFrame && ScriptedPhysicsFrame->PhysicsSteps == 1);
		PF_CHECK(Tests, ScriptedBodyTransform && ScriptedBodyTransform->Translation.y > 0.0f);
		ScriptedPhysicsRuntime.Stop();

		constexpr std::string_view UnavailableServicesScript = R"(
function OnUpdate()
  local applied, forceMessage = entity:apply_force(0, 1, 0)
  assert(applied == nil and string.find(forceMessage, "unavailable", 1, true) ~= nil)
  local played, audioMessage = entity:play_audio()
  assert(played == nil and string.find(audioMessage, "unavailable", 1, true) ~= nil)
  local pressed, inputMessage = input.is_key_pressed(87)
  assert(pressed == nil and string.find(inputMessage, "unavailable", 1, true) ~= nil)
end
)";
		const auto UnavailableServicesAsset = CreateScriptAsset("services_unavailable.lua", UnavailableServicesScript);
		PF_CHECK(Tests, UnavailableServicesAsset.has_value());
		if (!UnavailableServicesAsset)
			return;
		Scene UnavailableServicesScene;
		const auto UnavailableServicesEntity = UnavailableServicesScene.CreateEntity("No runtime services");
		PF_CHECK(Tests, UnavailableServicesEntity.has_value());
		if (!UnavailableServicesEntity)
			return;
		PF_CHECK(Tests, UnavailableServicesEntity->SetScript(ScriptComponent{ UnavailableServicesAsset->ID }).has_value());
		ScriptRuntime UnavailableServicesRuntime(*ProjectResult);
		PF_CHECK(Tests, UnavailableServicesRuntime.Start(UnavailableServicesScene).has_value());
		PF_CHECK(Tests, UnavailableServicesRuntime.Advance(UnavailableServicesScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, UnavailableServicesRuntime.GetDiagnostics().empty());
		UnavailableServicesRuntime.Stop();

		constexpr std::string_view InputScript = R"(
local Updated = false
function OnCreate()
  local invalidKey, keyMessage = input.is_key_pressed("W")
  assert(invalidKey == nil and keyMessage ~= nil)
  local invalidButton, buttonMessage = input.is_mouse_button_pressed("left")
  assert(invalidButton == nil and buttonMessage ~= nil)
end
function OnUpdate()
  local x, y = input.get_mouse_position()
  if not Updated then
    assert(input.is_key_pressed(87))
    assert(not input.is_key_pressed(83))
    assert(input.is_mouse_button_pressed(0))
    assert(not input.is_mouse_button_pressed(1))
    assert(x == 12.5 and y == 9.25)
    Updated = true
  else
    assert(not input.is_key_pressed(87))
    assert(not input.is_mouse_button_pressed(0))
    assert(x == 14.5 and y == 7.25)
  end
  assert(entity:set_translation(x, y, 0))
end
)";
		const auto InputScriptAsset = CreateScriptAsset("input.lua", InputScript);
		PF_CHECK(Tests, InputScriptAsset.has_value());
		if (!InputScriptAsset)
			return;

		Input TestInput;
		KeyPressedEvent WPressed(87, 0);
		MouseButtonPressedEvent LeftPressed(0);
		MouseMovedEvent InitialMousePosition(12.5f, 9.25f);
		TestInput.OnEvent(WPressed);
		TestInput.OnEvent(LeftPressed);
		TestInput.OnEvent(InitialMousePosition);

		Scene InputTestScene;
		const auto InputTestEntity = InputTestScene.CreateEntity("Input-driven script");
		PF_CHECK(Tests, InputTestEntity && InputTestEntity->SetScript(ScriptComponent{ InputScriptAsset->ID }));
		if (!InputTestEntity)
			return;
		ScriptRuntimeServices InputServices;
		InputServices.InputState = &TestInput;
		ScriptRuntime InputRuntime(*ProjectResult, {}, InputServices);
		PF_CHECK(Tests, InputRuntime.Start(InputTestScene).has_value());
		PF_CHECK(Tests, InputRuntime.Advance(InputTestScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, InputRuntime.GetDiagnostics().empty());
		KeyReleasedEvent WReleased(87);
		MouseButtonReleasedEvent LeftReleased(0);
		MouseMovedEvent UpdatedMousePosition(14.5f, 7.25f);
		TestInput.OnEvent(WReleased);
		TestInput.OnEvent(LeftReleased);
		TestInput.OnEvent(UpdatedMousePosition);
		PF_CHECK(Tests, InputRuntime.Advance(InputTestScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, InputRuntime.GetDiagnostics().empty());
		const auto InputUpdatedTransform = InputTestEntity->GetTransform();
		PF_CHECK(Tests, InputUpdatedTransform &&
			std::abs(InputUpdatedTransform->Translation.x - 14.5f) < 0.0001f &&
			std::abs(InputUpdatedTransform->Translation.y - 7.25f) < 0.0001f);
		InputRuntime.Stop();

		KeyPressedEvent CoordinatorWPressed(87, 0);
		MouseButtonPressedEvent CoordinatorLeftPressed(0);
		MouseMovedEvent CoordinatorMousePosition(12.5f, 9.25f);
		TestInput.OnEvent(CoordinatorWPressed);
		TestInput.OnEvent(CoordinatorLeftPressed);
		TestInput.OnEvent(CoordinatorMousePosition);
		Scene CoordinatedInputScene;
		const auto CoordinatedInputEntity = CoordinatedInputScene.CreateEntity("Coordinated input script");
		PF_CHECK(Tests, CoordinatedInputEntity &&
			CoordinatedInputEntity->SetScript(ScriptComponent{ InputScriptAsset->ID }));
		if (!CoordinatedInputEntity)
			return;
		SceneRuntimeServices HostServices{ .InputState = &TestInput };
		SceneRuntime CoordinatedInputRuntime(*ProjectResult, RuntimeDescription, HostServices);
		PF_CHECK(Tests, CoordinatedInputRuntime.Start(CoordinatedInputScene).has_value());
		PF_CHECK(Tests,
			CoordinatedInputRuntime.Advance(CoordinatedInputScene, Timestep(1.0 / 60.0)).has_value());
		const auto CoordinatedInputTransform = CoordinatedInputEntity->GetTransform();
		PF_CHECK(Tests, CoordinatedInputTransform &&
			std::abs(CoordinatedInputTransform->Translation.x - 12.5f) < 0.0001f &&
			std::abs(CoordinatedInputTransform->Translation.y - 9.25f) < 0.0001f);
		CoordinatedInputRuntime.Stop();

		Scene RollbackScene;
		const auto RollbackScriptEntity = RollbackScene.CreateEntity("Rollback script");
		const auto RollbackPhysicsEntity = RollbackScene.CreateEntity("Rollback physics entity");
		PF_CHECK(Tests, RollbackScriptEntity && RollbackPhysicsEntity);
		if (!RollbackScriptEntity || !RollbackPhysicsEntity)
			return;
		PF_CHECK(Tests, RollbackScriptEntity->SetScript(ScriptComponent{ TransformScriptAsset->ID }).has_value());
		PF_CHECK(Tests, RollbackPhysicsEntity->SetRigidbody(RigidbodyComponent{}).has_value());
		PF_CHECK(Tests, RollbackPhysicsEntity->SetBoxCollider(BoxColliderComponent{}).has_value());
		SceneRuntimeDesc InvalidScriptDescription = RuntimeDescription;
		InvalidScriptDescription.Scripting.MaxSourceBytes = 0;
		SceneRuntime RollbackRuntime(*ProjectResult, InvalidScriptDescription);
		const auto FailedRuntimeStart = RollbackRuntime.Start(RollbackScene);
		PF_CHECK(Tests,
			!FailedRuntimeStart && FailedRuntimeStart.error().Subsystem == SceneRuntimeSubsystem::Scripting);
		PF_CHECK(Tests, !RollbackRuntime.IsRunning());
		const auto RolledBackTransform = RollbackScriptEntity->GetTransform();
		PF_CHECK(Tests, RolledBackTransform && RolledBackTransform->Translation.x == 0.0f);

		constexpr std::string_view SyntaxErrorScript = "function OnUpdate(\n";
		const auto SyntaxErrorAsset = CreateScriptAsset("syntax_error.lua", SyntaxErrorScript);
		PF_CHECK(Tests, SyntaxErrorAsset.has_value());
		if (!SyntaxErrorAsset)
			return;
		Scene CompileErrorScene;
		const auto CompileErrorEntity = CompileErrorScene.CreateEntity("Compile error");
		PF_CHECK(Tests, CompileErrorEntity && CompileErrorEntity->SetScript(ScriptComponent{ SyntaxErrorAsset->ID }));
		ScriptRuntime CompileErrorRuntime(*ProjectResult);
		PF_CHECK(Tests, CompileErrorRuntime.Start(CompileErrorScene).has_value());
		PF_CHECK(Tests, CompileErrorRuntime.GetScriptCount() == 0 && CompileErrorRuntime.GetDiagnostics().size() == 1);
		PF_CHECK(Tests, CompileErrorRuntime.GetDiagnostics().front().Code == ScriptDiagnosticCode::CompileFailed);
		CompileErrorRuntime.Stop();
		PF_CHECK(Tests, CompileErrorRuntime.GetDiagnostics().size() == 1);
		CompileErrorRuntime.ClearDiagnostics();
		PF_CHECK(Tests, CompileErrorRuntime.GetDiagnostics().empty());

		constexpr std::string_view InfiniteScript = "function OnUpdate() while true do end end\n";
		const auto InfiniteScriptAsset = CreateScriptAsset("instruction_limit.lua", InfiniteScript);
		PF_CHECK(Tests, InfiniteScriptAsset.has_value());
		if (!InfiniteScriptAsset)
			return;
		Scene InfiniteScene;
		const auto InfiniteEntity = InfiniteScene.CreateEntity("Instruction-limited script");
		PF_CHECK(Tests, InfiniteEntity && InfiniteEntity->SetScript(ScriptComponent{ InfiniteScriptAsset->ID }));
		ScriptRuntimeDesc LimitedDescription;
		LimitedDescription.MaxInstructionsPerCallback = 1000;
		ScriptRuntime LimitedRuntime(*ProjectResult, LimitedDescription);
		PF_CHECK(Tests, LimitedRuntime.Start(InfiniteScene).has_value());
		PF_CHECK(Tests, LimitedRuntime.Advance(InfiniteScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, LimitedRuntime.GetDiagnostics().size() == 1);
		PF_CHECK(Tests, LimitedRuntime.GetDiagnostics().front().Code == ScriptDiagnosticCode::UpdateFailed);
		PF_CHECK(Tests, LimitedRuntime.GetDiagnostics().front().Message.find("instruction budget") != std::string::npos);
		PF_CHECK(Tests, LimitedRuntime.Advance(InfiniteScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, LimitedRuntime.GetDiagnostics().size() == 1);
		LimitedRuntime.Stop();

		constexpr std::string_view MemoryLimitScript = R"(
function OnCreate()
  local values = {}
  while true do
    values[#values + 1] = string.rep("x", 4096)
  end
end
)";
		const auto MemoryScriptAsset = CreateScriptAsset("memory_limit.lua", MemoryLimitScript);
		PF_CHECK(Tests, MemoryScriptAsset.has_value());
		if (!MemoryScriptAsset)
			return;
		Scene MemoryScene;
		const auto MemoryEntity = MemoryScene.CreateEntity("Memory-limited script");
		PF_CHECK(Tests, MemoryEntity && MemoryEntity->SetScript(ScriptComponent{ MemoryScriptAsset->ID }));
		ScriptRuntimeDesc MemoryDescription;
		MemoryDescription.MaxLuaMemoryBytes = 512u * 1024u;
		MemoryDescription.MaxTotalLuaMemoryBytes = 2u * 1024u * 1024u;
		ScriptRuntime MemoryRuntime(*ProjectResult, MemoryDescription);
		PF_CHECK(Tests, MemoryRuntime.Start(MemoryScene).has_value());
		PF_CHECK(Tests, MemoryRuntime.GetScriptCount() == 0 && MemoryRuntime.GetDiagnostics().size() == 1);
		PF_CHECK(Tests, MemoryRuntime.GetDiagnostics().front().Code == ScriptDiagnosticCode::InitializationFailed);
		MemoryRuntime.Stop();
	}
}

void RunProjectRuntimeTests(TestRunner& Tests)
{
	TestMaterialAssets(Tests);
	TestProjectFiles(Tests);
	TestEditorCommandLineAndStartupProjectLoading(Tests);
	TestBundledValidationProjectAssetRegistry(Tests);
	TestAssetReferenceValidation(Tests);
	TestScriptRuntime(Tests);
}
