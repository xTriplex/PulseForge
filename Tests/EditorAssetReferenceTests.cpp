#include "TestEditorSupport.h"

namespace
{
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
}

void RunEditorAssetReferenceTests(TestRunner& Tests)
{
	TestAssetReferenceValidation(Tests);
}
