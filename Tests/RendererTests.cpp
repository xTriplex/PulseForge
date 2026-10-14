#include "TestSupport.h"
#include "TestGraphicsSupport.h"

namespace
{
	using namespace PulseForgeTestSupport;
	void TestSceneSerializerClone(TestRunner& Tests)
	{
		using namespace PulseForge;
		Scene Source;
		const UUID RootIdentifier{ 0x7400000000000000ull, 74 };
		const UUID ChildIdentifier{ 0x7500000000000000ull, 75 };
		const AssetID MeshIdentifier{ 0x7600000000000000ull, 76 };
		const AssetID MaterialIdentifier{ 0x7700000000000000ull, 77 };
		auto Root = Source.CreateEntityWithUUID(RootIdentifier, "Runtime root");
		auto Child = Source.CreateEntityWithUUID(ChildIdentifier, "Runtime child");
		PF_CHECK(Tests, Root.has_value() && Child.has_value());
		if (!Root || !Child)
			return;

		TransformComponent RootTransform;
		RootTransform.Translation = { 2.0f, 3.0f, 4.0f };
		PF_CHECK(Tests, Root->SetTransform(RootTransform).has_value());
		PF_CHECK(Tests, Child->SetParent(*Root).has_value());
		PF_CHECK(Tests, Child->SetMeshRenderer(MeshRendererComponent{ MeshIdentifier, MaterialIdentifier }).has_value());

		auto Clone = SceneSerializer::Clone(Source);
		PF_CHECK(Tests, Clone.has_value() && Clone->get() != &Source);
		if (!Clone)
			return;

		PF_CHECK(Tests, (*Clone)->GetEntityCount() == Source.GetEntityCount());
		auto ClonedRoot = (*Clone)->FindEntity(RootIdentifier);
		auto ClonedChild = (*Clone)->FindEntity(ChildIdentifier);
		PF_CHECK(Tests, ClonedRoot.has_value() && ClonedChild.has_value());
		if (!ClonedRoot || !ClonedChild)
			return;

		const auto ClonedTransform = ClonedRoot->GetTransform();
		const auto ClonedRenderer = ClonedChild->GetMeshRenderer();
		const auto ClonedParent = ClonedChild->GetParent();
		PF_CHECK(Tests, ClonedTransform && glm::all(glm::equal(ClonedTransform->Translation, RootTransform.Translation)));
		PF_CHECK(Tests, ClonedRenderer && ClonedRenderer->has_value() &&
			ClonedRenderer->value().MeshAsset == MeshIdentifier && ClonedRenderer->value().MaterialAsset == MaterialIdentifier);
		PF_CHECK(Tests, ClonedParent && ClonedParent->has_value() && **ClonedParent == *ClonedRoot);

		TransformComponent RuntimeTransform = *ClonedTransform;
		RuntimeTransform.Translation.x = 20.0f;
		PF_CHECK(Tests, ClonedRoot->SetTransform(RuntimeTransform).has_value());
		const auto AuthoredTransform = Root->GetTransform();
		PF_CHECK(Tests, AuthoredTransform && AuthoredTransform->Translation.x == RootTransform.Translation.x);

		Scene Empty;
		auto EmptyClone = SceneSerializer::Clone(Empty);
		PF_CHECK(Tests, EmptyClone.has_value() && (*EmptyClone)->GetEntityCount() == 0);
	}

	void TestPrefabSerialization(TestRunner& Tests)
	{
		using namespace PulseForge;
		Scene Source;
		const auto ExternalParent = Source.CreateEntity("Outside prefab");
		const auto Root = Source.CreateEntityWithUUID(UUID{ 0x7000000000000000ull, 7 }, "Prefab root");
		const auto Child = Source.CreateEntityWithUUID(UUID{ 0x7100000000000000ull, 7 }, "Prefab child");
		const auto Grandchild = Source.CreateEntityWithUUID(UUID{ 0x7200000000000000ull, 7 }, "Prefab grandchild");
		PF_CHECK(Tests, ExternalParent && Root && Child && Grandchild);
		if (!ExternalParent || !Root || !Child || !Grandchild)
			return;

		PF_CHECK(Tests, Root->SetParent(*ExternalParent).has_value());
		PF_CHECK(Tests, Child->SetParent(*Root).has_value());
		PF_CHECK(Tests, Grandchild->SetParent(*Child).has_value());
		CameraComponent RootCamera;
		RootCamera.VerticalFieldOfViewRadians = 0.95f;
		RootCamera.IsPrimary = true;
		PF_CHECK(Tests, Root->SetCamera(RootCamera).has_value());
		const DirectionalLightComponent PrefabLight{ glm::vec3(0.8f, 0.9f, 1.0f), 1.75f };
		PF_CHECK(Tests, Root->SetDirectionalLight(PrefabLight).has_value());
		PointLightComponent PrefabPoint{ glm::vec3(1.0f, 0.5f, 0.25f), 3.0f, 9.0f };
		PrefabPoint.CastShadows = true;
		PrefabPoint.ShadowBias = 0.0025f;
		PrefabPoint.ShadowNormalBias = 0.04f;
		PrefabPoint.ShadowSoftness = 2.25f;
		PF_CHECK(Tests, Root->SetPointLight(PrefabPoint).has_value());
		SpotLightComponent PrefabSpot{ glm::vec3(0.3f, 0.5f, 1.0f), 7.0f, 14.0f, 8.0f, 22.0f };
		PrefabSpot.CastShadows = true;
		PrefabSpot.ShadowBias = 0.003f;
		PF_CHECK(Tests, Grandchild->SetSpotLight(PrefabSpot).has_value());
		const EnvironmentLightComponent PrefabEnvironment{ AssetID{ 0x7200000000000000ull, 7 }, 0.65f };
		PF_CHECK(Tests, Root->SetEnvironmentLight(PrefabEnvironment).has_value());
		const AssetID MeshAssetID{ 0x7300000000000000ull, 7 };
		const AssetID MaterialAssetID{ 0x7400000000000000ull, 7 };
		const AssetID AudioAssetID{ 0x7500000000000000ull, 7 };
		const AssetID ScriptAssetID{ 0x7600000000000000ull, 7 };
		PF_CHECK(Tests, Child->SetMeshRenderer(MeshRendererComponent{ MeshAssetID, MaterialAssetID }).has_value());
		PF_CHECK(Tests, Child->SetAudioSource(AudioSourceComponent{ AudioAssetID, 0.6f, true, false, true }).has_value());
		PF_CHECK(Tests, Child->SetScript(ScriptComponent{ ScriptAssetID, false }).has_value());
		PF_CHECK(Tests, Root->SetAudioListener(AudioListenerComponent{ true }).has_value());

		const auto PrefabData = PrefabSerializer::Serialize(Source, *Root);
		PF_CHECK(Tests, PrefabData.has_value());
		if (!PrefabData)
			return;
		PF_CHECK(Tests, PrefabData->find("\"format\": \"PulseForgePrefab\"") != std::string::npos);
		PF_CHECK(Tests, PrefabData->find("\"version\": 3") != std::string::npos);
		PF_CHECK(Tests, PrefabData->find("\"root\": \"" + Root->GetUUID().ToString() + "\"") != std::string::npos);
		PF_CHECK(Tests, PrefabData->find("\"meshAsset\": \"" + MeshAssetID.ToString() + "\"") != std::string::npos);
		PF_CHECK(Tests, PrefabData->find("\"materialAsset\": \"" + MaterialAssetID.ToString() + "\"") != std::string::npos);
		PF_CHECK(Tests, PrefabData->find("\"asset\": \"" + AudioAssetID.ToString() + "\"") != std::string::npos);
		PF_CHECK(Tests, PrefabData->find("\"asset\": \"" + ScriptAssetID.ToString() + "\"") != std::string::npos);
		PF_CHECK(Tests, PrefabData->find("\"primary\": false") != std::string::npos);
		PF_CHECK(Tests, PrefabData->find("\"primary\": true") == std::string::npos);
		PF_CHECK(Tests, PrefabData->find("\"directionalLight\"") != std::string::npos);
		PF_CHECK(Tests, PrefabData->find("\"pointLight\"") != std::string::npos);
		PF_CHECK(Tests, PrefabData->find("\"spotLight\"") != std::string::npos);
		PF_CHECK(Tests, PrefabData->find("\"environmentLight\"") != std::string::npos);
		const auto SourceCamera = Root->GetCamera();
		const auto SourceListener = Root->GetAudioListener();
		PF_CHECK(Tests, SourceCamera && SourceCamera->has_value() && SourceCamera->value().IsPrimary);
		PF_CHECK(Tests, SourceListener && SourceListener->has_value() && SourceListener->value().IsPrimary);
		PF_CHECK(Tests, PrefabData->find("Outside prefab") == std::string::npos);
		PF_CHECK(Tests, PrefabData->find(ExternalParent->GetUUID().ToString()) == std::string::npos);

		std::string PrimaryRolePrefab = *PrefabData;
		size_t PrimaryRoleCount = 0;
		for (size_t PrimaryFlag = PrimaryRolePrefab.find("\"primary\": false");
			PrimaryFlag != std::string::npos;
			PrimaryFlag = PrimaryRolePrefab.find("\"primary\": false", PrimaryFlag))
		{
			PrimaryRolePrefab.replace(PrimaryFlag, std::string("\"primary\": false").size(), "\"primary\": true");
			PrimaryFlag += std::string("\"primary\": true").size();
			++PrimaryRoleCount;
		}
		PF_CHECK(Tests, PrimaryRoleCount == 2);
		Scene PrimaryRoleDestination;
		const auto PrimaryRoleInstance = PrefabSerializer::Instantiate(PrimaryRolePrefab, PrimaryRoleDestination);
		PF_CHECK(Tests, PrimaryRoleInstance.has_value());
		if (PrimaryRoleInstance)
		{
			const auto InstanceCamera = PrimaryRoleInstance->GetCamera();
			const auto InstanceListener = PrimaryRoleInstance->GetAudioListener();
			PF_CHECK(Tests, InstanceCamera && InstanceCamera->has_value() && !InstanceCamera->value().IsPrimary);
			PF_CHECK(Tests, InstanceListener && InstanceListener->has_value() && !InstanceListener->value().IsPrimary);
		}

		std::string LegacyPrefabV1 = *PrefabData;
		const size_t CameraPrimaryPosition = LegacyPrefabV1.find("\"primary\": false");
		PF_CHECK(Tests, CameraPrimaryPosition != std::string::npos);
		if (CameraPrimaryPosition != std::string::npos)
		{
			LegacyPrefabV1.replace(CameraPrimaryPosition, std::string("\"primary\": false").size(), "\"primary\": true");
			const size_t PrimaryCommaPosition = LegacyPrefabV1.rfind(',', CameraPrimaryPosition);
			const size_t PrimaryLineEnd = LegacyPrefabV1.find('\n', CameraPrimaryPosition);
			PF_CHECK(Tests, PrimaryCommaPosition != std::string::npos && PrimaryLineEnd != std::string::npos);
			if (PrimaryCommaPosition != std::string::npos && PrimaryLineEnd != std::string::npos)
				LegacyPrefabV1.erase(PrimaryCommaPosition, PrimaryLineEnd - PrimaryCommaPosition);
		}
		const size_t EmbeddedSceneVersionPosition = LegacyPrefabV1.find("\"version\": 7", LegacyPrefabV1.find("\"scene\""));
		PF_CHECK(Tests, EmbeddedSceneVersionPosition != std::string::npos);
		if (EmbeddedSceneVersionPosition != std::string::npos)
			LegacyPrefabV1.replace(EmbeddedSceneVersionPosition, std::string("\"version\": 7").size(), "\"version\": 3");
		const size_t OuterPrefabVersionPosition = LegacyPrefabV1.find("\"version\": 3");
		PF_CHECK(Tests, OuterPrefabVersionPosition != std::string::npos);
		if (OuterPrefabVersionPosition != std::string::npos)
			LegacyPrefabV1.replace(OuterPrefabVersionPosition, std::string("\"version\": 3").size(), "\"version\": 1");
		Scene LegacyPrefabDestination;
		const auto LegacyPrefabInstance = PrefabSerializer::Instantiate(LegacyPrefabV1, LegacyPrefabDestination);
		PF_CHECK(Tests, LegacyPrefabInstance.has_value());
		if (LegacyPrefabInstance)
		{
			const auto LegacyCamera = LegacyPrefabInstance->GetCamera();
			PF_CHECK(Tests, LegacyCamera && LegacyCamera->has_value() && !LegacyCamera->value().IsPrimary);
		}

		Scene Destination;
		const auto Existing = Destination.CreateEntity("Existing destination entity");
		PF_CHECK(Tests, Existing.has_value());
		const size_t OriginalEntityCount = Destination.GetEntityCount();
		const auto FirstInstance = PrefabSerializer::Instantiate(*PrefabData, Destination);
		PF_CHECK(Tests, FirstInstance.has_value());
		PF_CHECK(Tests, Destination.GetEntityCount() == OriginalEntityCount + 3);
		if (!FirstInstance)
			return;
		PF_CHECK(Tests, FirstInstance->GetUUID() != Root->GetUUID());
		const auto FirstParent = FirstInstance->GetParent();
		PF_CHECK(Tests, FirstParent && !FirstParent->has_value());
		const auto FirstCamera = FirstInstance->GetCamera();
		const auto FirstLight = FirstInstance->GetDirectionalLight();
		const auto FirstEnvironment = FirstInstance->GetEnvironmentLight();
		const auto FirstPointLight = FirstInstance->GetPointLight();
		const auto FirstAudioListener = FirstInstance->GetAudioListener();
		PF_CHECK(Tests, FirstCamera && FirstCamera->has_value() &&
			FirstCamera->value().VerticalFieldOfViewRadians == RootCamera.VerticalFieldOfViewRadians);
		PF_CHECK(Tests, FirstCamera && FirstCamera->has_value() && !FirstCamera->value().IsPrimary);
		PF_CHECK(Tests, FirstLight && FirstLight->has_value() &&
			FirstLight->value().Intensity == PrefabLight.Intensity);
		PF_CHECK(Tests, FirstEnvironment && FirstEnvironment->has_value() &&
			FirstEnvironment->value().HdrImage == PrefabEnvironment.HdrImage &&
			FirstEnvironment->value().Intensity == PrefabEnvironment.Intensity);
		PF_CHECK(Tests, FirstPointLight && FirstPointLight->has_value() &&
			FirstPointLight->value().Range == 9.0f && FirstPointLight->value().CastShadows &&
			FirstPointLight->value().ShadowBias == PrefabPoint.ShadowBias &&
			FirstPointLight->value().ShadowNormalBias == PrefabPoint.ShadowNormalBias &&
			FirstPointLight->value().ShadowSoftness == PrefabPoint.ShadowSoftness);
		PF_CHECK(Tests, FirstAudioListener && FirstAudioListener->has_value() && !FirstAudioListener->value().IsPrimary);
		const auto FirstChildren = FirstInstance->GetChildren();
		PF_CHECK(Tests, FirstChildren && FirstChildren->size() == 1);
		if (!FirstChildren || FirstChildren->size() != 1)
			return;
		const Entity FirstChild = FirstChildren->front();
		const auto FirstChildTag = FirstChild.GetTag();
		const auto FirstChildMesh = FirstChild.GetMeshRenderer();
		const auto FirstChildAudioSource = FirstChild.GetAudioSource();
		const auto FirstChildScript = FirstChild.GetScript();
		const auto FirstGrandchildren = FirstChild.GetChildren();
		PF_CHECK(Tests, FirstChildTag && FirstChildTag->Name == "Prefab child");
		PF_CHECK(Tests, FirstChildMesh && FirstChildMesh->has_value() && FirstChildMesh->value().MeshAsset == MeshAssetID);
		PF_CHECK(Tests, FirstChildMesh && FirstChildMesh->has_value() && FirstChildMesh->value().MaterialAsset == MaterialAssetID);
		PF_CHECK(Tests, FirstChildAudioSource && FirstChildAudioSource->has_value() &&
			FirstChildAudioSource->value().AudioAsset == AudioAssetID && !FirstChildAudioSource->value().PlayOnStart);
		PF_CHECK(Tests, FirstChildScript && FirstChildScript->has_value() &&
			FirstChildScript->value().ScriptAsset == ScriptAssetID && !FirstChildScript->value().Enabled);
		PF_CHECK(Tests, FirstGrandchildren && FirstGrandchildren->size() == 1);
		if (FirstGrandchildren && FirstGrandchildren->size() == 1)
		{
			const auto FirstGrandchildSpot = FirstGrandchildren->front().GetSpotLight();
			PF_CHECK(Tests, FirstGrandchildSpot && FirstGrandchildSpot->has_value() &&
				FirstGrandchildSpot->value().OuterConeAngleDegrees == 22.0f &&
				FirstGrandchildSpot->value().CastShadows && FirstGrandchildSpot->value().ShadowBias == 0.003f);
		}

		const auto SecondInstance = PrefabSerializer::Instantiate(*PrefabData, Destination);
		PF_CHECK(Tests, SecondInstance.has_value());
		PF_CHECK(Tests, Destination.GetEntityCount() == OriginalEntityCount + 6);
		PF_CHECK(Tests, SecondInstance && FirstInstance->GetUUID() != SecondInstance->GetUUID());
		if (SecondInstance)
		{
			const auto SecondCamera = SecondInstance->GetCamera();
			const auto SecondListener = SecondInstance->GetAudioListener();
			PF_CHECK(Tests, SecondCamera && SecondCamera->has_value() && !SecondCamera->value().IsPrimary);
			PF_CHECK(Tests, SecondListener && SecondListener->has_value() && !SecondListener->value().IsPrimary);
		}

		const size_t CountBeforeFailure = Destination.GetEntityCount();
		const auto InvalidJSON = PrefabSerializer::Instantiate("{ invalid", Destination);
		PF_CHECK(Tests, !InvalidJSON.has_value());
		PF_CHECK(Tests, !InvalidJSON && InvalidJSON.error().Code == PrefabErrorCode::InvalidDocument);
		PF_CHECK(Tests, Destination.GetEntityCount() == CountBeforeFailure);

		std::string MissingPrefabRoot = *PrefabData;
		const size_t RootFieldPosition = MissingPrefabRoot.find("\"root\": \"");
		const size_t RootIdentifierPosition = MissingPrefabRoot.find(Root->GetUUID().ToString(), RootFieldPosition);
		PF_CHECK(Tests, RootFieldPosition != std::string::npos && RootIdentifierPosition != std::string::npos);
		if (RootIdentifierPosition != std::string::npos)
			MissingPrefabRoot.replace(
				RootIdentifierPosition,
				Root->GetUUID().ToString().size(),
				UUID{ 0x7400000000000000ull, 7 }.ToString());
		const auto MissingRootResult = PrefabSerializer::Instantiate(MissingPrefabRoot, Destination);
		PF_CHECK(Tests, !MissingRootResult.has_value());
		PF_CHECK(Tests, !MissingRootResult && MissingRootResult.error().Code == PrefabErrorCode::InvalidDocument);
		PF_CHECK(Tests, Destination.GetEntityCount() == CountBeforeFailure);

		std::string UnsupportedVersion = *PrefabData;
		const size_t VersionPosition = UnsupportedVersion.find("\"version\": 3");
		PF_CHECK(Tests, VersionPosition != std::string::npos);
		if (VersionPosition != std::string::npos)
			UnsupportedVersion.replace(VersionPosition, std::string("\"version\": 3").size(), "\"version\": 99");
		const auto UnsupportedPrefabVersion = PrefabSerializer::Instantiate(UnsupportedVersion, Destination);
		PF_CHECK(Tests, !UnsupportedPrefabVersion.has_value());
		PF_CHECK(Tests, !UnsupportedPrefabVersion &&
			UnsupportedPrefabVersion.error().Code == PrefabErrorCode::UnsupportedVersion);
		PF_CHECK(Tests, Destination.GetEntityCount() == CountBeforeFailure);

		Scene OtherScene;
		const auto ForeignRoot = OtherScene.CreateEntityWithUUID(Root->GetUUID(), "Foreign prefab root");
		PF_CHECK(Tests, ForeignRoot.has_value());
		if (ForeignRoot)
		{
			const auto ForeignPrefab = PrefabSerializer::Serialize(Source, *ForeignRoot);
			PF_CHECK(Tests, !ForeignPrefab.has_value());
			PF_CHECK(Tests, !ForeignPrefab && ForeignPrefab.error().Code == PrefabErrorCode::InvalidRootEntity);
		}
	}

	void TestPrefabAssetsByUUID(TestRunner& Tests)
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

		const std::filesystem::path ProjectRoot = TemporaryDirectory / ("PulseForgePrefabAssets-" + ProjectIdentifier->ToString());
		struct ProjectCleanup
		{
			std::filesystem::path Path;
			~ProjectCleanup()
			{
				std::error_code Error;
				std::filesystem::remove_all(Path, Error);
			}
		} Cleanup{ ProjectRoot };

		const std::filesystem::path PrefabDirectory = ProjectRoot / "Assets" / "Prefabs";
		std::filesystem::create_directories(PrefabDirectory, FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError)
			return;

		const auto RootID = UUID::Parse("15261902-2c12-4a92-9e99-0e355a16af4e");
		const auto ChildID = UUID::Parse("df753d9d-1024-40f1-980f-5ca589fd87d8");
		const auto MeshAssetID = UUID::Parse("d3ce3630-b87a-4dab-b8e0-59c1f258ae78");
		PF_CHECK(Tests, RootID && ChildID && MeshAssetID);
		if (!RootID || !ChildID || !MeshAssetID)
			return;

		Scene Source;
		const auto Root = Source.CreateEntityWithUUID(*RootID, "Prefab Root");
		const auto Child = Source.CreateEntityWithUUID(*ChildID, "Prefab Child");
		PF_CHECK(Tests, Root && Child);
		if (!Root || !Child)
			return;
		PF_CHECK(Tests, Child->SetParent(*Root).has_value());
		PF_CHECK(Tests, Child->SetMeshRenderer(MeshRendererComponent{ *MeshAssetID }).has_value());

		const std::filesystem::path SourcePath = PrefabDirectory / "crate.prefab";
		PF_CHECK(Tests, PrefabSerializer::SaveToFile(Source, *Root, SourcePath).has_value());
		const auto Metadata = AssetMetadataSerializer::CreateForNewAsset(SourcePath);
		PF_CHECK(Tests, Metadata.has_value());
		if (!Metadata)
			return;

		AssetRegistry Registry;
		PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());
		Scene Destination;
		const auto Existing = Destination.CreateEntity("Existing entity");
		PF_CHECK(Tests, Existing.has_value());
		const auto FirstInstance = PrefabAssetService::Instantiate(Metadata->ID, ProjectRoot, Registry, Destination);
		PF_CHECK(Tests, FirstInstance.has_value());
		PF_CHECK(Tests, FirstInstance && FirstInstance->GetUUID() != *RootID);
		PF_CHECK(Tests, Destination.GetEntityCount() == 3);
		if (!FirstInstance)
			return;
		const auto FirstChildren = FirstInstance->GetChildren();
		PF_CHECK(Tests, FirstChildren && FirstChildren->size() == 1);
		if (!FirstChildren || FirstChildren->size() != 1)
			return;
		const auto ChildRenderer = FirstChildren->front().GetMeshRenderer();
		PF_CHECK(Tests, ChildRenderer && ChildRenderer->has_value());
		PF_CHECK(Tests, ChildRenderer && ChildRenderer->has_value() && ChildRenderer->value().MeshAsset == *MeshAssetID);

		const auto MoveResult = AssetOperations::Move(
			Registry,
			ProjectRoot,
			Metadata->ID,
			"Assets/Prefabs/renamed.prefab");
		PF_CHECK(Tests, MoveResult && MoveResult->ID == Metadata->ID);
		PF_CHECK(Tests, PrefabAssetService::Instantiate(Metadata->ID, ProjectRoot, Registry, Destination).has_value());

		auto RootTag = Root->GetTag();
		PF_CHECK(Tests, RootTag.has_value());
		if (!RootTag)
			return;
		RootTag->Name = "Saved through prefab UUID";
		PF_CHECK(Tests, Root->SetTag(*RootTag).has_value());
		PF_CHECK(Tests, PrefabAssetService::Save(Metadata->ID, ProjectRoot, Registry, Source, *Root).has_value());
		const auto PreservedMetadata = AssetMetadataSerializer::LoadFromFile(
			AssetMetadataSerializer::GetSidecarPath(ProjectRoot / "Assets" / "Prefabs" / "renamed.prefab"));
		PF_CHECK(Tests, PreservedMetadata && PreservedMetadata->ID == Metadata->ID);
		Scene UpdatedInstance;
		const auto UpdatedRoot = PrefabAssetService::Instantiate(Metadata->ID, ProjectRoot, Registry, UpdatedInstance);
		PF_CHECK(Tests, UpdatedRoot.has_value());
		if (UpdatedRoot)
		{
			const auto UpdatedTag = UpdatedRoot->GetTag();
			PF_CHECK(Tests, UpdatedTag && UpdatedTag->Name == "Saved through prefab UUID");
		}

		const auto CreatedPrefab = PrefabAssetService::Create(
			Registry,
			ProjectRoot,
			"Assets/Prefabs/generated.prefab",
			Source,
			*Root);
		PF_CHECK(Tests, CreatedPrefab.has_value());
		PF_CHECK(Tests, CreatedPrefab && CreatedPrefab->ID != Metadata->ID &&
			CreatedPrefab->ProjectRelativePath == "Assets/Prefabs/generated.prefab");
		if (CreatedPrefab)
		{
			const auto CreatedMetadata = AssetMetadataSerializer::LoadFromFile(
				ProjectRoot / "Assets/Prefabs/generated.prefab.meta");
			PF_CHECK(Tests, CreatedMetadata && CreatedMetadata->ID == CreatedPrefab->ID);
			PF_CHECK(Tests, Registry.Find(CreatedPrefab->ID).has_value());

			Scene CreatedPrefabScene;
			const auto CreatedRoot = PrefabAssetService::Instantiate(
				CreatedPrefab->ID,
				ProjectRoot,
				Registry,
				CreatedPrefabScene);
			PF_CHECK(Tests, CreatedRoot && CreatedRoot->GetUUID() != Root->GetUUID());
			PF_CHECK(Tests, CreatedPrefabScene.GetEntityCount() == 2);
			if (CreatedRoot)
			{
				const auto CreatedChildren = CreatedRoot->GetChildren();
				PF_CHECK(Tests, CreatedChildren && CreatedChildren->size() == 1);
			}

			const auto DuplicateCreation = PrefabAssetService::Create(
				Registry,
				ProjectRoot,
				"Assets/Prefabs/generated.prefab",
				Source,
				*Root);
			PF_CHECK(Tests, !DuplicateCreation &&
				DuplicateCreation.error().Code == PrefabAssetErrorCode::AssetOperationFailed);
			PF_CHECK(Tests, Registry.Find(CreatedPrefab->ID).has_value());
		}
		const auto InvalidPrefabCreation = PrefabAssetService::Create(
			Registry,
			ProjectRoot,
			"Assets/Prefabs/generated.json",
			Source,
			*Root);
		PF_CHECK(Tests, !InvalidPrefabCreation &&
			InvalidPrefabCreation.error().Code == PrefabAssetErrorCode::UnsupportedAssetType);

		const auto Duplicate = AssetOperations::Duplicate(
			Registry,
			ProjectRoot,
			Metadata->ID,
			"Assets/Prefabs/copy.prefab");
		PF_CHECK(Tests, Duplicate.has_value());
		PF_CHECK(Tests, Duplicate && Duplicate->ID != Metadata->ID);
		PF_CHECK(Tests, Duplicate && PrefabAssetService::Instantiate(Duplicate->ID, ProjectRoot, Registry, UpdatedInstance).has_value());

		const std::filesystem::path OtherAssetPath = PrefabDirectory / "notes.txt";
		{
			std::ofstream Output(OtherAssetPath, std::ios::binary | std::ios::trunc);
			Output << "not a prefab";
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto OtherMetadata = AssetMetadataSerializer::CreateForNewAsset(OtherAssetPath);
		PF_CHECK(Tests, OtherMetadata.has_value());
		PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());
		if (OtherMetadata)
		{
			const auto WrongType = PrefabAssetService::Instantiate(OtherMetadata->ID, ProjectRoot, Registry, UpdatedInstance);
			PF_CHECK(Tests, !WrongType && WrongType.error().Code == PrefabAssetErrorCode::UnsupportedAssetType);
		}

		const std::filesystem::path BrokenPath = PrefabDirectory / "broken.prefab";
		{
			std::ofstream Output(BrokenPath, std::ios::binary | std::ios::trunc);
			Output << "{broken";
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto BrokenMetadata = AssetMetadataSerializer::CreateForNewAsset(BrokenPath);
		PF_CHECK(Tests, BrokenMetadata.has_value());
		PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());
		if (BrokenMetadata)
		{
			const size_t CountBeforeFailure = UpdatedInstance.GetEntityCount();
			const auto Broken = PrefabAssetService::Instantiate(BrokenMetadata->ID, ProjectRoot, Registry, UpdatedInstance);
			PF_CHECK(Tests, !Broken && Broken.error().Code == PrefabAssetErrorCode::SerializationFailed);
			PF_CHECK(Tests, UpdatedInstance.GetEntityCount() == CountBeforeFailure);
		}

		const auto MissingID = UUID::Generate();
		PF_CHECK(Tests, MissingID.has_value());
		if (MissingID)
		{
			const auto Missing = PrefabAssetService::Instantiate(*MissingID, ProjectRoot, Registry, UpdatedInstance);
			PF_CHECK(Tests, !Missing && Missing.error().Code == PrefabAssetErrorCode::AssetNotFound);
		}
	}

	void TestGraphicsPipelineAndDrawValidation(TestRunner& Tests)
	{
		using namespace PulseForge;
		GraphicsPipelineDesc Pipeline = MakeTestPipelineDescription();
		PF_CHECK(Tests, ValidateGraphicsPipelineDescription(Pipeline).has_value());
		PF_CHECK(Tests, Pipeline.VertexShader != nullptr && Pipeline.FragmentShader != nullptr);

		auto MissingFragmentShader = Pipeline;
		MissingFragmentShader.FragmentShader.reset();
		PF_CHECK(Tests, !ValidateGraphicsPipelineDescription(MissingFragmentShader).has_value());

		auto WrongShaderStage = Pipeline;
		WrongShaderStage.VertexShader = std::make_shared<TestShader>(ShaderStage::Fragment);
		PF_CHECK(Tests, !ValidateGraphicsPipelineDescription(WrongShaderStage).has_value());

		auto UnsupportedTopology = Pipeline;
		UnsupportedTopology.Topology = static_cast<PrimitiveTopology>(0xff);
		PF_CHECK(Tests, !ValidateGraphicsPipelineDescription(UnsupportedTopology).has_value());

		auto UnsupportedTarget = Pipeline;
		UnsupportedTarget.ColorFormat = static_cast<ColorTargetFormat>(0xff);
		PF_CHECK(Tests, !ValidateGraphicsPipelineDescription(UnsupportedTarget).has_value());

		auto UnormTarget = Pipeline;
		UnormTarget.ColorFormat = ColorTargetFormat::RGBA8_UNorm;
		PF_CHECK(Tests, ValidateGraphicsPipelineDescription(UnormTarget).has_value());
		auto SrgbTarget = Pipeline;
		SrgbTarget.ColorFormat = ColorTargetFormat::RGBA8_Srgb;
		PF_CHECK(Tests, ValidateGraphicsPipelineDescription(SrgbTarget).has_value());
		auto HalfFloatPipeline = Pipeline;
		HalfFloatPipeline.ColorFormat = ColorTargetFormat::RGBA16_Float;
		PF_CHECK(Tests, ValidateGraphicsPipelineDescription(HalfFloatPipeline).has_value());
		HalfFloatPipeline.DepthAttachmentEnabled = false;
		PF_CHECK(Tests, ValidateGraphicsPipelineDescription(HalfFloatPipeline).has_value());
		HalfFloatPipeline.DepthAttachmentEnabled = true;
		HalfFloatPipeline.Depth.TestEnabled = true;
		HalfFloatPipeline.Depth.WriteEnabled = true;
		PF_CHECK(Tests, ValidateGraphicsPipelineDescription(HalfFloatPipeline).has_value());

		auto DepthTestedPipeline = Pipeline;
		DepthTestedPipeline.Depth.TestEnabled = true;
		DepthTestedPipeline.Depth.WriteEnabled = true;
		PF_CHECK(Tests, ValidateGraphicsPipelineDescription(DepthTestedPipeline).has_value());

		auto InvalidDepthComparison = DepthTestedPipeline;
		InvalidDepthComparison.Depth.Compare = static_cast<DepthCompareOperation>(0xff);
		PF_CHECK(Tests, !ValidateGraphicsPipelineDescription(InvalidDepthComparison).has_value());

		auto DepthWriteWithoutTest = Pipeline;
		DepthWriteWithoutTest.Depth.WriteEnabled = true;
		PF_CHECK(Tests, !ValidateGraphicsPipelineDescription(DepthWriteWithoutTest).has_value());

		auto DepthOnlyPipeline = Pipeline;
		DepthOnlyPipeline.FragmentShader.reset();
		DepthOnlyPipeline.ColorFormat = ColorTargetFormat::None;
		DepthOnlyPipeline.Depth.TestEnabled = true;
		DepthOnlyPipeline.Depth.WriteEnabled = true;
		PF_CHECK(Tests, ValidateGraphicsPipelineDescription(DepthOnlyPipeline).has_value());
		auto DepthOnlyWithColor = DepthOnlyPipeline;
		DepthOnlyWithColor.ColorFormat = ColorTargetFormat::RGBA8_UNorm;
		PF_CHECK(Tests, !ValidateGraphicsPipelineDescription(DepthOnlyWithColor).has_value());
		auto DepthOnlyWithoutTesting = DepthOnlyPipeline;
		DepthOnlyWithoutTesting.Depth.TestEnabled = false;
		PF_CHECK(Tests, !ValidateGraphicsPipelineDescription(DepthOnlyWithoutTesting).has_value());
		auto DepthOnlyWithoutWrites = DepthOnlyPipeline;
		DepthOnlyWithoutWrites.Depth.WriteEnabled = false;
		PF_CHECK(Tests, !ValidateGraphicsPipelineDescription(DepthOnlyWithoutWrites).has_value());
		auto ColorPipelineWithoutFragment = Pipeline;
		ColorPipelineWithoutFragment.FragmentShader.reset();
		PF_CHECK(Tests, !ValidateGraphicsPipelineDescription(ColorPipelineWithoutFragment).has_value());
		auto DepthOnlyBlending = DepthOnlyPipeline;
		DepthOnlyBlending.Blend.Enabled = true;
		PF_CHECK(Tests, !ValidateGraphicsPipelineDescription(DepthOnlyBlending).has_value());
		auto ColorOnlyPipeline = Pipeline;
		ColorOnlyPipeline.DepthAttachmentEnabled = false;
		PF_CHECK(Tests, ValidateGraphicsPipelineDescription(ColorOnlyPipeline).has_value());
		auto ColorOnlyWithDepthTest = ColorOnlyPipeline;
		ColorOnlyWithDepthTest.Depth.TestEnabled = true;
		PF_CHECK(Tests, !ValidateGraphicsPipelineDescription(ColorOnlyWithDepthTest).has_value());
		auto NonFiniteBias = DepthOnlyPipeline;
		NonFiniteBias.Rasterizer.SlopeScaledDepthBias = std::numeric_limits<float>::infinity();
		PF_CHECK(Tests, !ValidateGraphicsPipelineDescription(NonFiniteBias).has_value());

		BufferDesc VertexBuffer{ sizeof(float) * 6 * 3, BufferUsage::Vertex, "Test triangle vertices" };
		DrawArguments Triangle{ 3, 1, 0, 0 };
		PF_CHECK(Tests, ValidateDrawArguments(Triangle, Pipeline, VertexBuffer).has_value());

		auto ZeroVertices = Triangle;
		ZeroVertices.VertexCount = 0;
		PF_CHECK(Tests, !ValidateDrawArguments(ZeroVertices, Pipeline, VertexBuffer).has_value());

		auto BeyondBuffer = Triangle;
		BeyondBuffer.VertexCount = 4;
		PF_CHECK(Tests, !ValidateDrawArguments(BeyondBuffer, Pipeline, VertexBuffer).has_value());

		auto IndexBuffer = VertexBuffer;
		IndexBuffer.Usage = BufferUsage::Index;
		PF_CHECK(Tests, !ValidateDrawArguments(Triangle, Pipeline, IndexBuffer).has_value());

		auto ZeroInstances = Triangle;
		ZeroInstances.InstanceCount = 0;
		PF_CHECK(Tests, !ValidateDrawArguments(ZeroInstances, Pipeline, VertexBuffer).has_value());

		BufferDesc IndexedVertexBuffer{ sizeof(float) * 6 * 3, BufferUsage::Vertex, "Raw indexed draw vertices" };
		BufferDesc RawIndexBuffer{ sizeof(uint32_t) * 6, BufferUsage::Index, "Raw indexed draw indices" };
		const DrawIndexedArguments RawIndexedTriangle{ 3, 1, 0, 0 };
		PF_CHECK(Tests, ValidateIndexedBufferDrawArguments(
			RawIndexedTriangle,
			Pipeline,
			IndexedVertexBuffer,
			RawIndexBuffer).has_value());

		auto IndexRangePastEnd = RawIndexedTriangle;
		IndexRangePastEnd.FirstIndex = 4;
		PF_CHECK(Tests, !ValidateIndexedBufferDrawArguments(
			IndexRangePastEnd,
			Pipeline,
			IndexedVertexBuffer,
			RawIndexBuffer).has_value());

		auto WrongRawIndexBuffer = RawIndexBuffer;
		WrongRawIndexBuffer.Usage = BufferUsage::Vertex;
		PF_CHECK(Tests, !ValidateIndexedBufferDrawArguments(
			RawIndexedTriangle,
			Pipeline,
			IndexedVertexBuffer,
			WrongRawIndexBuffer).has_value());

		auto MisalignedIndexBuffer = RawIndexBuffer;
		++MisalignedIndexBuffer.ByteSize;
		PF_CHECK(Tests, !ValidateIndexedBufferDrawArguments(
			RawIndexedTriangle,
			Pipeline,
			IndexedVertexBuffer,
			MisalignedIndexBuffer).has_value());

		auto ScissoredPipeline = Pipeline;
		ScissoredPipeline.Rasterizer.ScissorEnabled = true;
		auto ScissoredDraw = RawIndexedTriangle;
		ScissoredDraw.Scissor = ScissorRect{ 10, 12, 64, 48 };
		PF_CHECK(Tests, ValidateIndexedBufferDrawArguments(
			ScissoredDraw,
			ScissoredPipeline,
			IndexedVertexBuffer,
			RawIndexBuffer).has_value());

		auto MissingScissorState = Pipeline;
		PF_CHECK(Tests, !ValidateIndexedBufferDrawArguments(
			ScissoredDraw,
			MissingScissorState,
			IndexedVertexBuffer,
			RawIndexBuffer).has_value());

		auto EmptyScissor = ScissoredDraw;
		EmptyScissor.Scissor->Width = 0;
		PF_CHECK(Tests, !ValidateIndexedBufferDrawArguments(
			EmptyScissor,
			ScissoredPipeline,
			IndexedVertexBuffer,
			RawIndexBuffer).has_value());
	}

	void TestToneMappingMathAndOutputEncoding(TestRunner& Tests)
	{
		using namespace PulseForge;
		ToneMappingSettings Settings;
		PF_CHECK(Tests, Settings.ExposureEV == 0.0f);
		PF_CHECK(Tests, IsValidToneMappingSettings(Settings));
		PF_CHECK(Tests, TryUpdateToneMappingSettings(Settings, { -16.0f }));
		PF_CHECK(Tests, Settings.ExposureEV == -16.0f);
		PF_CHECK(Tests, TryUpdateToneMappingSettings(Settings, { 16.0f }));
		PF_CHECK(Tests, TryUpdateToneMappingSettings(Settings, { 0.0f }));
		PF_CHECK(Tests, TryUpdateToneMappingSettings(Settings, { -2.0f }));
		PF_CHECK(Tests, TryUpdateToneMappingSettings(Settings, { -1.0f }));
		PF_CHECK(Tests, TryUpdateToneMappingSettings(Settings, { 1.0f }));
		PF_CHECK(Tests, TryUpdateToneMappingSettings(Settings, { 2.0f }));
		for (float Invalid : { std::numeric_limits<float>::quiet_NaN(),
			std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(), -16.01f, 16.01f })
		{
			const ToneMappingSettings Previous = Settings;
			PF_CHECK(Tests, !TryUpdateToneMappingSettings(Settings, { Invalid }));
			PF_CHECK(Tests, Settings.ExposureEV == Previous.ExposureEV);
		}

		const auto Black = ApplyAcesFittedToneMapping({ 0.0f, -1.0f, -10.0f }, 0.0f);
		PF_CHECK(Tests, Black[0] == 0.0f && Black[1] == 0.0f && Black[2] == 0.0f);
		const auto NonFinite = ApplyAcesFittedToneMapping({
			std::numeric_limits<float>::infinity(),
			-std::numeric_limits<float>::infinity(),
			std::numeric_limits<float>::quiet_NaN() }, 0.0f);
		PF_CHECK(Tests, NonFinite[0] == 1.0f && NonFinite[1] == 0.0f && NonFinite[2] == 0.0f);
		const auto Reference = ApplyAcesFittedToneMapping({ 0.18f, 1.0f, 4.0f }, 0.0f);
		PF_CHECK(Tests, std::abs(Reference[0] - 0.2669f) < 0.0002f);
		PF_CHECK(Tests, std::abs(Reference[1] - 0.803797f) < 0.00001f);
		PF_CHECK(Tests, std::abs(Reference[2] - 0.973417f) < 0.00002f);
		const auto NegativeExposure = ApplyAcesFittedToneMapping({ 1.0f, 1.0f, 1.0f }, -1.0f);
		const auto PositiveExposure = ApplyAcesFittedToneMapping({ 1.0f, 1.0f, 1.0f }, 1.0f);
		PF_CHECK(Tests, NegativeExposure[0] < Reference[1] && PositiveExposure[0] > Reference[1]);
		PF_CHECK(Tests, PositiveExposure[0] < 1.0f);
		const auto Large = ApplyAcesFittedToneMapping({ (std::numeric_limits<float>::max)(), 1.0e20f, 65504.0f }, 16.0f);
		PF_CHECK(Tests, Large[0] == 1.0f && Large[1] == 1.0f && Large[2] == 1.0f);
		float Previous = -1.0f;
		for (float Value : { 0.0f, 0.01f, 0.1f, 0.18f, 0.5f, 1.0f, 2.0f, 16.0f, 65504.0f })
		{
			const auto Mapped = ApplyAcesFittedToneMapping({ Value, Value, Value }, 0.0f);
			PF_CHECK(Tests, std::isfinite(Mapped[0]) && Mapped[0] >= 0.0f && Mapped[0] <= 1.0f);
			PF_CHECK(Tests, Mapped[0] >= Previous);
			Previous = Mapped[0];
		}
		PF_CHECK(Tests, LinearToSrgb(0.0f) == 0.0f);
		PF_CHECK(Tests, std::abs(LinearToSrgb(0.0031308f) - 0.0404499f) < 1.0e-5f);
		PF_CHECK(Tests, std::abs(LinearToSrgb(0.04045f) - 0.222205f) < 1.0e-5f);
		PF_CHECK(Tests, LinearToSrgb(1.0f) == 1.0f);
		PF_CHECK(Tests, ShouldShaderEncodeSrgb(ColorTargetFormat::RGBA8_UNorm, OutputColorEncoding::SrgbAttachment));
		PF_CHECK(Tests, !ShouldShaderEncodeSrgb(ColorTargetFormat::RGBA8_Srgb, OutputColorEncoding::UnormAttachment));
		PF_CHECK(Tests, !ShouldShaderEncodeSrgb(ColorTargetFormat::Swapchain, OutputColorEncoding::SrgbAttachment));
		PF_CHECK(Tests, ShouldShaderEncodeSrgb(ColorTargetFormat::Swapchain, OutputColorEncoding::UnormAttachment));
		PF_CHECK(Tests, !IsToneMappingBindingCurrent(0, 0));
		PF_CHECK(Tests, IsToneMappingBindingCurrent(5, 5));
		PF_CHECK(Tests, !IsToneMappingBindingCurrent(4, 5));
	}

	void TestConstantBufferBindingValidation(TestRunner& Tests)
	{
		using namespace PulseForge;
		BindingLayoutDesc LayoutDescription;
		LayoutDescription.Visibility = ShaderVisibility::Fragment;
		LayoutDescription.Items = { { BindingResourceType::ConstantBuffer, 0 } };
		LayoutDescription.DebugName = "Test fragment constants";
		PF_CHECK(Tests, ValidateBindingLayout(LayoutDescription).has_value());

		auto DuplicateSlot = LayoutDescription;
		DuplicateSlot.Items.push_back({ BindingResourceType::ConstantBuffer, 0 });
		PF_CHECK(Tests, !ValidateBindingLayout(DuplicateSlot).has_value());

		auto InvalidVisibility = LayoutDescription;
		InvalidVisibility.Visibility = static_cast<ShaderVisibility>(0xff);
		PF_CHECK(Tests, !ValidateBindingLayout(InvalidVisibility).has_value());

		auto SharedVisibility = LayoutDescription;
		SharedVisibility.Visibility = ShaderVisibility::AllGraphics;
		PF_CHECK(Tests, ValidateBindingLayout(SharedVisibility).has_value());

		auto UnsupportedType = LayoutDescription;
		UnsupportedType.Items[0].Type = static_cast<BindingResourceType>(0xff);
		PF_CHECK(Tests, !ValidateBindingLayout(UnsupportedType).has_value());

		BindingLayoutHandle Layout = std::make_shared<TestBindingLayout>(LayoutDescription);
		TestBuffer ConstantBuffer(BufferDesc{ 16, BufferUsage::Constant, "Test constants" });
		TestBuffer VertexBuffer(BufferDesc{ 48, BufferUsage::Vertex, "Test vertices" });
		BindingSetDesc SetDescription;
		SetDescription.Layout = Layout;
		SetDescription.Buffers.push_back({ 0, std::cref(static_cast<const Buffer&>(ConstantBuffer)) });
		PF_CHECK(Tests, ValidateBindingSet(SetDescription).has_value());

		auto MissingBuffer = SetDescription;
		MissingBuffer.Buffers.clear();
		PF_CHECK(Tests, !ValidateBindingSet(MissingBuffer).has_value());

		auto WrongBufferType = SetDescription;
		WrongBufferType.Buffers[0].Resource = std::cref(static_cast<const Buffer&>(VertexBuffer));
		PF_CHECK(Tests, !ValidateBindingSet(WrongBufferType).has_value());

		auto WrongSlot = SetDescription;
		WrongSlot.Buffers[0].Slot = 1;
		PF_CHECK(Tests, !ValidateBindingSet(WrongSlot).has_value());

		GraphicsPipelineDesc Pipeline = MakeTestPipelineDescription();
		Pipeline.BindingLayouts.push_back(Layout);
		PF_CHECK(Tests, ValidateGraphicsPipelineDescription(Pipeline).has_value());
		auto DuplicateRegisterSpace = Pipeline;
		DuplicateRegisterSpace.BindingLayouts.push_back(Layout);
		PF_CHECK(Tests, !ValidateGraphicsPipelineDescription(DuplicateRegisterSpace).has_value());

		TestBindingSet CreatedTestBindingSet(Layout);
		const std::array<const BindingSet*, 1> ValidSets = { &CreatedTestBindingSet };
		PF_CHECK(Tests, ValidateDrawBindingSets(Pipeline, ValidSets).has_value());
		PF_CHECK(Tests, !ValidateDrawBindingSets(Pipeline, {}).has_value());

		BindingLayoutHandle DifferentLayout = std::make_shared<TestBindingLayout>(LayoutDescription);
		TestBindingSet MismatchedSet(DifferentLayout);
		const std::array<const BindingSet*, 1> MismatchedSets = { &MismatchedSet };
		PF_CHECK(Tests, !ValidateDrawBindingSets(Pipeline, MismatchedSets).has_value());
	}

	void TestScreenSpaceAmbientOcclusionMath(TestRunner& Tests)
	{
		using namespace PulseForge;
		const AmbientOcclusionKernel Kernel = GenerateAmbientOcclusionKernel();
		const AmbientOcclusionKernel RepeatedKernel = GenerateAmbientOcclusionKernel();
		for (size_t Index = 0; Index < Kernel.Samples.size(); ++Index)
		{
			const glm::vec4& Sample = Kernel.Samples[Index];
			PF_CHECK(Tests, std::isfinite(Sample.x) && std::isfinite(Sample.y) &&
				std::isfinite(Sample.z) && Sample.z >= 0.0f);
			PF_CHECK(Tests, glm::length(glm::vec3(Sample)) <= 1.0f + 1.0e-5f);
			PF_CHECK(Tests, Sample == RepeatedKernel.Samples[Index]);
		}
		PF_CHECK(Tests, (CalculateAmbientOcclusionExtent(1920, 1080) == std::array<uint32_t, 2>{ 960, 540 }));
		PF_CHECK(Tests, (CalculateAmbientOcclusionExtent(5, 3) == std::array<uint32_t, 2>{ 3, 2 }));
		PF_CHECK(Tests, (CalculateAmbientOcclusionExtent(1, 1) == std::array<uint32_t, 2>{ 1, 1 }));
		PF_CHECK(Tests, CalculateAmbientOcclusionExtent(std::numeric_limits<uint32_t>::max(), 9)[0] ==
			std::numeric_limits<uint32_t>::max() / 2u + 1u);

		const glm::mat4 Projection = glm::perspective(glm::radians(60.0f), 1.6f, 0.1f, 100.0f);
		const glm::mat4 InverseProjection = glm::inverse(Projection);
		for (const glm::vec3 ViewPosition : { glm::vec3(0.0f, 0.0f, -2.0f), glm::vec3(0.5f, -0.3f, -5.0f) })
		{
			const glm::vec4 Clip = Projection * glm::vec4(ViewPosition, 1.0f);
			const glm::vec3 Ndc = glm::vec3(Clip) / Clip.w;
			const glm::vec2 UV{ Ndc.x * 0.5f + 0.5f, 0.5f - Ndc.y * 0.5f };
			const auto Reconstructed = ReconstructViewPosition(InverseProjection, UV, Ndc.z);
			PF_CHECK(Tests, Reconstructed && glm::length(*Reconstructed - ViewPosition) < 1.0e-4f);
		}
		PF_CHECK(Tests, !ReconstructViewPosition(InverseProjection, glm::vec2(0.5f), 1.1f));

		for (const glm::vec3 Normal : { glm::vec3(1, 0, 0), glm::vec3(-1, 0, 0), glm::vec3(0, 1, 0),
			glm::vec3(0, -1, 0), glm::vec3(0, 0, 1), glm::vec3(0, 0, -1), glm::normalize(glm::vec3(1, 2, 3)) })
		{
			const glm::vec3 Decoded = DecodeViewNormal(EncodeViewNormal(Normal));
			PF_CHECK(Tests, glm::length(Decoded - Normal) < 1.0e-5f);
		}
		PF_CHECK(Tests, AmbientOcclusionRangeWeight(0.0f, 0.6f) == 1.0f);
		PF_CHECK(Tests, AmbientOcclusionRangeWeight(0.6f, 0.6f) == 0.0f);
		PF_CHECK(Tests, AmbientOcclusionRangeWeight(std::numeric_limits<float>::infinity(), 0.6f) == 0.0f);
		PF_CHECK(Tests, ComputeSpecularOcclusion(1.0f, 0.2f, 0.8f) == 1.0f);
		PF_CHECK(Tests, ComputeSpecularOcclusion(0.2f, 1.0f, 0.05f) == 1.0f);
		PF_CHECK(Tests, ComputeSpecularOcclusion(0.2f, 0.0f, 1.0f) < 0.21f);
	}

	void TestEnvironmentLightingProcessing(TestRunner& Tests)
	{
		using namespace PulseForge;
		const glm::vec2 PositiveX = EquirectangularUvFromDirection({ 1.0f, 0.0f, 0.0f });
		const glm::vec2 NegativeX = EquirectangularUvFromDirection({ -1.0f, 0.0f, 0.0f });
		const glm::vec2 PositiveZ = EquirectangularUvFromDirection({ 0.0f, 0.0f, 1.0f });
		const glm::vec2 NegativeZ = EquirectangularUvFromDirection({ 0.0f, 0.0f, -1.0f });
		const glm::vec2 PositiveY = EquirectangularUvFromDirection({ 0.0f, 1.0f, 0.0f });
		const glm::vec2 NegativeY = EquirectangularUvFromDirection({ 0.0f, -1.0f, 0.0f });
		PF_CHECK(Tests, std::abs(PositiveX.x - 0.5f) < 0.0001f && std::abs(PositiveX.y - 0.5f) < 0.0001f);
		PF_CHECK(Tests, NegativeX.x < 0.0001f || NegativeX.x > 0.9999f);
		PF_CHECK(Tests, std::abs(PositiveZ.x - 0.75f) < 0.0001f);
		PF_CHECK(Tests, std::abs(NegativeZ.x - 0.25f) < 0.0001f);
		PF_CHECK(Tests, std::abs(PositiveY.y) < 0.0001f && std::abs(NegativeY.y - 1.0f) < 0.0001f);

		constexpr std::array<glm::vec3, 6> ExpectedFaceCenters = {
			glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(-1.0f, 0.0f, 0.0f),
			glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f),
			glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, 0.0f, -1.0f)
		};
		for (uint32_t Face = 0; Face < ExpectedFaceCenters.size(); ++Face)
		{
			const glm::vec3 Direction = CubeFaceTexelDirection(static_cast<CubeFace>(Face), 0.5f, 0.5f);
			PF_CHECK(Tests, glm::dot(Direction, ExpectedFaceCenters[Face]) > 0.9999f);
		}

		ImportedFloatImage ConstantEnvironment;
		ConstantEnvironment.Width = 8;
		ConstantEnvironment.Height = 4;
		ConstantEnvironment.RGBA32FPixels.resize(8 * 4 * 4);
		for (size_t Pixel = 0; Pixel < 8 * 4; ++Pixel)
		{
			ConstantEnvironment.RGBA32FPixels[Pixel * 4] = 2.0f;
			ConstantEnvironment.RGBA32FPixels[Pixel * 4 + 1] = 0.5f;
			ConstantEnvironment.RGBA32FPixels[Pixel * 4 + 2] = 0.25f;
			ConstantEnvironment.RGBA32FPixels[Pixel * 4 + 3] = 1.0f;
		}
		EnvironmentProcessingDesc Settings;
		Settings.EnvironmentFaceSize = 8;
		Settings.IrradianceFaceSize = 4;
		Settings.SpecularFaceSize = 8;
		Settings.BrdfLutSize = 8;
		Settings.IrradianceSamples = 32;
		Settings.PrefilterSamples = 32;
		Settings.BrdfSamples = 32;
		auto Processed = ProcessEnvironmentImage(ConstantEnvironment, Settings);
		PF_CHECK(Tests, Processed.has_value());
		if (Processed)
		{
			PF_CHECK(Tests, Processed->Environment.Size == 8 && Processed->DiffuseIrradiance.Size == 4);
			PF_CHECK(Tests, Processed->PrefilteredSpecular.size() == 4);
			PF_CHECK(Tests, Processed->PrefilteredSpecular.front().Size == 8 && Processed->PrefilteredSpecular.back().Size == 1);
			PF_CHECK(Tests, Processed->BrdfIntegrationLut.Width == 8 && Processed->BrdfIntegrationLut.Height == 8);
			for (const float Channel : Processed->Environment.Faces[0])
				PF_CHECK(Tests, std::isfinite(Channel));
			for (const float Channel : Processed->BrdfIntegrationLut.RGBA32FPixels)
				PF_CHECK(Tests, std::isfinite(Channel));
			const std::vector<float>& IrradiancePositiveX = Processed->DiffuseIrradiance.Faces[0];
			PF_CHECK(Tests, std::abs(IrradiancePositiveX[0] - 2.0f * std::numbers::pi_v<float>) < 0.05f);
			PF_CHECK(Tests, std::abs(IrradiancePositiveX[1] - 0.5f * std::numbers::pi_v<float>) < 0.05f);
			for (const FloatCubeLevel& Level : Processed->PrefilteredSpecular)
			{
				PF_CHECK(Tests, std::abs(Level.Faces[0][0] - 2.0f) < 0.01f);
				PF_CHECK(Tests, std::abs(Level.Faces[0][1] - 0.5f) < 0.01f);
			}
		}
		std::stop_source CancelSource;
		CancelSource.request_stop();
		const auto CancelledProcessing = ProcessEnvironmentImage(ConstantEnvironment, Settings, CancelSource.get_token());
		PF_CHECK(Tests, !CancelledProcessing &&
			CancelledProcessing.error().Code == EnvironmentProcessingErrorCode::Cancelled);
		auto InvalidSize = Settings;
		InvalidSize.EnvironmentFaceSize = 7;
		PF_CHECK(Tests, !ProcessEnvironmentImage(ConstantEnvironment, InvalidSize).has_value());
		ConstantEnvironment.RGBA32FPixels[0] = std::numeric_limits<float>::quiet_NaN();
		PF_CHECK(Tests, !ProcessEnvironmentImage(ConstantEnvironment, Settings).has_value());
	}

	void TestTextureAndSamplerValidation(TestRunner& Tests)
	{
		using namespace PulseForge;
		TextureDesc Description;
		Description.Width = 2;
		Description.Height = 2;
		Description.Format = TextureFormat::RGBA8_Srgb;
		PF_CHECK(Tests, ValidateTextureUpload(Description, 16).value() == 16);
		PF_CHECK(Tests, !ValidateTextureUpload(Description, 15).has_value());
		PF_CHECK(Tests, !ValidateTextureUpload(Description, 17).has_value());
		PF_CHECK(Tests, Description.Dimension == TextureDimension::Texture2D && Description.MipLevels == 1);
		PF_CHECK(Tests, GetTextureFormatBytesPerPixel(TextureFormat::RGBA8_UNorm) == 4);
		PF_CHECK(Tests, GetTextureFormatBytesPerPixel(TextureFormat::RGBA8_Srgb) == 4);
		PF_CHECK(Tests, GetTextureFormatBytesPerPixel(TextureFormat::RGBA16_Float) == 8);
		PF_CHECK(Tests, GetTextureFormatBytesPerPixel(TextureFormat::RGBA32_Float) == 16);
		PF_CHECK(Tests, !GetTextureFormatBytesPerPixel(TextureFormat::Depth32Float).has_value());

		TextureDesc HalfFloatDescription;
		HalfFloatDescription.Format = TextureFormat::RGBA16_Float;
		HalfFloatDescription.Width = 1;
		HalfFloatDescription.Height = 1;
		PF_CHECK(Tests, ValidateTextureUpload(HalfFloatDescription, 8).value() == 8);
		PF_CHECK(Tests, !ValidateTextureUpload(HalfFloatDescription, 7).has_value());
		PF_CHECK(Tests, !ValidateTextureUpload(HalfFloatDescription, 9).has_value());
		HalfFloatDescription.Width = 0;
		PF_CHECK(Tests, !ValidateTextureUpload(HalfFloatDescription, 0).has_value());
		HalfFloatDescription.Width = 2;
		HalfFloatDescription.Height = 3;
		PF_CHECK(Tests, ValidateTextureUpload(HalfFloatDescription, 48).value() == 48);
		PF_CHECK(Tests, !ValidateTextureUpload(HalfFloatDescription, 47).has_value());
		HalfFloatDescription.Width = 4;
		HalfFloatDescription.Height = 4;
		PF_CHECK(Tests, ValidateTextureUpload(HalfFloatDescription, 128).value() == 128);
		PF_CHECK(Tests, !ValidateTextureUpload(HalfFloatDescription, 127).has_value());

		const std::array<std::byte, 72> HalfFloatUploadBytes{};
		HalfFloatDescription.Width = 2;
		HalfFloatDescription.Height = 3;
		const TextureSubresourceData HalfFloatSubresource{ 0, 0, HalfFloatUploadBytes, 24 };
		PF_CHECK(Tests, ValidateTextureUpload(HalfFloatDescription,
			std::span<const TextureSubresourceData>(&HalfFloatSubresource, 1)).has_value());
		const TextureSubresourceData InvalidHalfFloatPitch{ 0, 0, HalfFloatUploadBytes, 15 };
		PF_CHECK(Tests, !ValidateTextureUpload(HalfFloatDescription,
			std::span<const TextureSubresourceData>(&InvalidHalfFloatPitch, 1)).has_value());
		const TextureSubresourceData InvalidHalfFloatSize{ 0, 0,
			std::span<const std::byte>(HalfFloatUploadBytes).first(71), 24 };
		PF_CHECK(Tests, !ValidateTextureUpload(HalfFloatDescription,
			std::span<const TextureSubresourceData>(&InvalidHalfFloatSize, 1)).has_value());
		const std::array<std::byte, 73> OversizedHalfFloatUploadBytes{};
		const TextureSubresourceData OversizedHalfFloatSubresource{ 0, 0, OversizedHalfFloatUploadBytes, 24 };
		PF_CHECK(Tests, !ValidateTextureUpload(HalfFloatDescription,
			std::span<const TextureSubresourceData>(&OversizedHalfFloatSubresource, 1)).has_value());
		HalfFloatDescription.Width = 4;
		HalfFloatDescription.Height = 4;
		HalfFloatDescription.MipLevels = 3;
		const std::array<std::byte, 168> HalfFloatMips{};
		const std::array<TextureSubresourceData, 3> HalfFloatMipSubresources = {
			TextureSubresourceData{ 0, 0, std::span(HalfFloatMips).first(128), 0 },
			TextureSubresourceData{ 1, 0, std::span(HalfFloatMips).subspan(128, 32), 0 },
			TextureSubresourceData{ 2, 0, std::span(HalfFloatMips).last(8), 0 }
		};
		PF_CHECK(Tests, ValidateTextureUpload(HalfFloatDescription, HalfFloatMipSubresources).has_value());
		PF_CHECK(Tests, !ValidateTextureUpload(HalfFloatDescription,
			std::span(HalfFloatMipSubresources).first(2)).has_value());
		HalfFloatDescription.MipLevels = 4;
		PF_CHECK(Tests, !ValidateTextureUpload(HalfFloatDescription,
			std::span<const TextureSubresourceData>{}).has_value());
		HalfFloatDescription.MipLevels = 1;
		HalfFloatDescription.Usage = TextureUsage::ShaderResource | TextureUsage::ColorAttachment;
		PF_CHECK(Tests, ValidateTextureUpload(HalfFloatDescription, 0).value() == 0);
		HalfFloatDescription.Usage = TextureUsage::ColorAttachment;
		PF_CHECK(Tests, ValidateTextureUpload(HalfFloatDescription, 0).value() == 0);
		HalfFloatDescription.Usage = TextureUsage::DepthStencilAttachment;
		PF_CHECK(Tests, !ValidateTextureUpload(HalfFloatDescription, 0).has_value());
		HalfFloatDescription.Usage = static_cast<TextureUsage>(0x80);
		PF_CHECK(Tests, !ValidateTextureUpload(HalfFloatDescription,
			std::span<const TextureSubresourceData>{}).has_value());
		HalfFloatDescription.Usage = TextureUsage::ShaderResource;
		HalfFloatDescription.Dimension = TextureDimension::TextureCube;
		HalfFloatDescription.Width = 1;
		HalfFloatDescription.Height = 1;
		std::array<std::byte, 48> HalfFloatCubeBytes{};
		std::array<TextureSubresourceData, 6> HalfFloatCube{};
		for (uint32_t Face = 0; Face < HalfFloatCube.size(); ++Face)
			HalfFloatCube[Face] = { 0, Face, std::span(HalfFloatCubeBytes).subspan(Face * 8, 8), 0 };
		PF_CHECK(Tests, ValidateTextureUpload(HalfFloatDescription, HalfFloatCube).has_value());
		HalfFloatDescription.Usage = TextureUsage::ShaderResource | TextureUsage::ColorAttachment;
		PF_CHECK(Tests, !ValidateTextureUpload(HalfFloatDescription, HalfFloatCube).has_value());

		TextureDesc CubeDescription;
		CubeDescription.Width = 1;
		CubeDescription.Height = 1;
		CubeDescription.Format = TextureFormat::RGBA32_Float;
		CubeDescription.Dimension = TextureDimension::TextureCube;
		std::array<std::byte, 96> CubeUploadBytes{};
		std::array<TextureSubresourceData, 6> CubeSubresources{};
		for (uint32_t Face = 0; Face < CubeSubresources.size(); ++Face)
			CubeSubresources[Face] = { 0, Face, std::span(CubeUploadBytes).subspan(Face * 16, 16), 0 };
		PF_CHECK(Tests, ValidateTextureUpload(CubeDescription, CubeSubresources).has_value());
		PF_CHECK(Tests, !ValidateTextureUpload(CubeDescription, std::span(CubeSubresources).first(5)).has_value());
		auto DuplicateFaceUploads = CubeSubresources;
		DuplicateFaceUploads[5].ArraySlice = 4;
		PF_CHECK(Tests, !ValidateTextureUpload(CubeDescription, DuplicateFaceUploads).has_value());
		auto InvalidFaceUpload = CubeSubresources;
		InvalidFaceUpload[0].ArraySlice = 6;
		PF_CHECK(Tests, !ValidateTextureUpload(CubeDescription, InvalidFaceUpload).has_value());
		auto InvalidCubePitch = CubeSubresources;
		InvalidCubePitch[0].RowPitch = 15;
		PF_CHECK(Tests, !ValidateTextureUpload(CubeDescription, InvalidCubePitch).has_value());
		auto RectangularCube = CubeDescription;
		RectangularCube.Height = 2;
		PF_CHECK(Tests, !ValidateTextureUpload(RectangularCube, CubeSubresources).has_value());
		auto ExcessMips = CubeDescription;
		ExcessMips.MipLevels = 2;
		PF_CHECK(Tests, !ValidateTextureUpload(ExcessMips, CubeSubresources).has_value());

		TextureDesc MippedFloat2D = CubeDescription;
		MippedFloat2D.Dimension = TextureDimension::Texture2D;
		MippedFloat2D.Width = 2;
		MippedFloat2D.Height = 2;
		MippedFloat2D.MipLevels = 2;
		const std::array<std::byte, 80> MippedUploadBytes{};
		const std::array<TextureSubresourceData, 2> MippedSubresources = {
			TextureSubresourceData{ 0, 0, std::span(MippedUploadBytes).first(64), 0 },
			TextureSubresourceData{ 1, 0, std::span(MippedUploadBytes).last(16), 0 }
		};
		PF_CHECK(Tests, ValidateTextureUpload(MippedFloat2D, MippedSubresources).has_value());
		auto MissingMipUpload = MippedSubresources;
		MissingMipUpload[1].MipLevel = 0;
		PF_CHECK(Tests, !ValidateTextureUpload(MippedFloat2D, MissingMipUpload).has_value());

		auto ZeroWidth = Description;
		ZeroWidth.Width = 0;
		PF_CHECK(Tests, !ValidateTextureUpload(ZeroWidth, 0).has_value());

		auto UnsupportedFormat = Description;
		UnsupportedFormat.Format = static_cast<TextureFormat>(0xff);
		PF_CHECK(Tests, !ValidateTextureUpload(UnsupportedFormat, 16).has_value());

		auto OverflowDimensions = Description;
		OverflowDimensions.Width = UINT32_MAX;
		OverflowDimensions.Height = UINT32_MAX;
		PF_CHECK(Tests, !ValidateTextureUpload(OverflowDimensions, 0).has_value());
		auto HalfFloatOverflowDimensions = OverflowDimensions;
		HalfFloatOverflowDimensions.Format = TextureFormat::RGBA16_Float;
		PF_CHECK(Tests, !ValidateTextureUpload(HalfFloatOverflowDimensions, 0).has_value());

		TextureDesc DepthDescription;
		DepthDescription.Width = 128;
		DepthDescription.Height = 64;
		DepthDescription.Format = TextureFormat::Depth32Float;
		DepthDescription.Usage = TextureUsage::DepthStencilAttachment;
		PF_CHECK(Tests, ValidateTextureUpload(DepthDescription, 0).value() == 0);

		auto ZeroDepthHeight = DepthDescription;
		ZeroDepthHeight.Height = 0;
		PF_CHECK(Tests, !ValidateTextureUpload(ZeroDepthHeight, 0).has_value());

		auto DepthAsShaderResource = DepthDescription;
		DepthAsShaderResource.Usage = TextureUsage::ShaderResource;
		PF_CHECK(Tests, !ValidateTextureUpload(DepthAsShaderResource, 0).has_value());

		PF_CHECK(Tests, !ValidateTextureUpload(DepthDescription, sizeof(float)).has_value());

		auto ColorAsDepthAttachment = Description;
		ColorAsDepthAttachment.Usage = TextureUsage::DepthStencilAttachment;
		PF_CHECK(Tests, !ValidateTextureUpload(ColorAsDepthAttachment, 16).has_value());

		auto RenderableColor = Description;
		RenderableColor.Usage = TextureUsage::ShaderResource | TextureUsage::ColorAttachment;
		PF_CHECK(Tests, ValidateTextureUpload(RenderableColor, 0).value() == 0);
		PF_CHECK(Tests, ValidateTextureUpload(RenderableColor, 16).value() == 16);
		PF_CHECK(Tests, !ValidateTextureUpload(RenderableColor, 15).has_value());

		auto AttachmentOnly = Description;
		AttachmentOnly.Usage = TextureUsage::ColorAttachment;
		PF_CHECK(Tests, ValidateTextureUpload(AttachmentOnly, 0).value() == 0);

		auto MissingShaderUpload = Description;
		PF_CHECK(Tests, !ValidateTextureUpload(MissingShaderUpload, 0).has_value());

		auto UnknownUsage = Description;
		UnknownUsage.Usage = static_cast<TextureUsage>(0x80);
		PF_CHECK(Tests, !ValidateTextureUpload(UnknownUsage, 16).has_value());

		auto SampleableDepth = DepthDescription;
		SampleableDepth.Usage = TextureUsage::DepthStencilAttachment | TextureUsage::ShaderResource;
		PF_CHECK(Tests, ValidateTextureUpload(SampleableDepth, 0).has_value());
		auto SampleableDepthCube = SampleableDepth;
		SampleableDepthCube.Dimension = TextureDimension::TextureCube;
		SampleableDepthCube.Width = 512;
		SampleableDepthCube.Height = 512;
		PF_CHECK(Tests, ValidateTextureUpload(SampleableDepthCube, std::span<const TextureSubresourceData>{}).has_value());
		auto NonSquareDepthCube = SampleableDepthCube;
		NonSquareDepthCube.Width = 256;
		PF_CHECK(Tests, !ValidateTextureUpload(NonSquareDepthCube, std::span<const TextureSubresourceData>{}).has_value());
		auto MippedDepthCube = SampleableDepthCube;
		MippedDepthCube.MipLevels = 2;
		PF_CHECK(Tests, !ValidateTextureUpload(MippedDepthCube, std::span<const TextureSubresourceData>{}).has_value());
		auto InvalidDepthUsage = DepthDescription;
		InvalidDepthUsage.Usage = TextureUsage::DepthStencilAttachment | TextureUsage::ColorAttachment;
		PF_CHECK(Tests, !ValidateTextureUpload(InvalidDepthUsage, 0).has_value());

		RenderTargetDesc TargetDescription;
		TargetDescription.Width = 800;
		TargetDescription.Height = 600;
		PF_CHECK(Tests, ValidateRenderTargetDescription(TargetDescription).has_value());
		PF_CHECK(Tests, MatchesRenderTargetConfiguration(
			TargetDescription, 800, 600, ColorTargetFormat::RGBA8_Srgb, DepthAttachmentMode::Attachment));
		PF_CHECK(Tests, !MatchesRenderTargetConfiguration(
			TargetDescription, 0, 600, ColorTargetFormat::RGBA8_Srgb, DepthAttachmentMode::Attachment));
		PF_CHECK(Tests, !MatchesRenderTargetConfiguration(
			TargetDescription, 800, 600, ColorTargetFormat::RGBA16_Float, DepthAttachmentMode::Attachment));
		PF_CHECK(Tests, !MatchesRenderTargetConfiguration(
			TargetDescription, 800, 600, ColorTargetFormat::RGBA8_Srgb, DepthAttachmentMode::None));
		auto HalfFloatColorTarget = TargetDescription;
		HalfFloatColorTarget.ColorFormat = ColorTargetFormat::RGBA16_Float;
		HalfFloatColorTarget.DepthMode = DepthAttachmentMode::None;
		PF_CHECK(Tests, ValidateRenderTargetDescription(HalfFloatColorTarget).has_value());
		PF_CHECK(Tests, MatchesRenderTargetConfiguration(
			HalfFloatColorTarget, 800, 600, ColorTargetFormat::RGBA16_Float, DepthAttachmentMode::None));
		HalfFloatColorTarget.DepthMode = DepthAttachmentMode::Attachment;
		PF_CHECK(Tests, ValidateRenderTargetDescription(HalfFloatColorTarget).has_value());
		PF_CHECK(Tests, MatchesRenderTargetConfiguration(
			HalfFloatColorTarget, 800, 600, ColorTargetFormat::RGBA16_Float, DepthAttachmentMode::Attachment));
		auto DepthOnlyTarget = TargetDescription;
		DepthOnlyTarget.ColorFormat = ColorTargetFormat::None;
		PF_CHECK(Tests, ValidateRenderTargetDescription(DepthOnlyTarget).has_value());
		PF_CHECK(Tests, DepthOnlyTarget.DepthMode == DepthAttachmentMode::Attachment);
		auto SampleableDepthOnlyTarget = DepthOnlyTarget;
		SampleableDepthOnlyTarget.DepthMode = DepthAttachmentMode::ShaderReadableAttachment;
		PF_CHECK(Tests, ValidateRenderTargetDescription(SampleableDepthOnlyTarget).has_value());
		auto ColorOnlyTarget = TargetDescription;
		ColorOnlyTarget.DepthMode = DepthAttachmentMode::None;
		PF_CHECK(Tests, ValidateRenderTargetDescription(ColorOnlyTarget).has_value());
		auto ColorSampleableDepthTarget = TargetDescription;
		ColorSampleableDepthTarget.DepthMode = DepthAttachmentMode::ShaderReadableAttachment;
		PF_CHECK(Tests, ValidateRenderTargetDescription(ColorSampleableDepthTarget).has_value());
		auto NoAttachmentsTarget = ColorOnlyTarget;
		NoAttachmentsTarget.ColorFormat = ColorTargetFormat::None;
		PF_CHECK(Tests, !ValidateRenderTargetDescription(NoAttachmentsTarget).has_value());
		auto InvalidDepthMode = TargetDescription;
		InvalidDepthMode.DepthMode = static_cast<DepthAttachmentMode>(0xff);
		PF_CHECK(Tests, !ValidateRenderTargetDescription(InvalidDepthMode).has_value());
		auto ZeroTargetWidth = TargetDescription;
		ZeroTargetWidth.Width = 0;
		PF_CHECK(Tests, !ValidateRenderTargetDescription(ZeroTargetWidth).has_value());
		auto ZeroTargetHeight = TargetDescription;
		ZeroTargetHeight.Height = 0;
		PF_CHECK(Tests, !ValidateRenderTargetDescription(ZeroTargetHeight).has_value());
		auto SwapchainTarget = TargetDescription;
		SwapchainTarget.ColorFormat = ColorTargetFormat::Swapchain;
		PF_CHECK(Tests, !ValidateRenderTargetDescription(SwapchainTarget).has_value());
		auto UnknownTargetFormat = TargetDescription;
		UnknownTargetFormat.ColorFormat = static_cast<ColorTargetFormat>(0xff);
		PF_CHECK(Tests, !ValidateRenderTargetDescription(UnknownTargetFormat).has_value());

		RenderTargetClearValue ClearValue;
		PF_CHECK(Tests, ValidateRenderTargetClearValue(ClearValue).has_value());
		auto InvalidClearDepth = ClearValue;
		InvalidClearDepth.Depth = 1.1f;
		PF_CHECK(Tests, !ValidateRenderTargetClearValue(InvalidClearDepth).has_value());
		auto NonFiniteClearColor = ClearValue;
		NonFiniteClearColor.Color[2] = std::numeric_limits<float>::infinity();
		PF_CHECK(Tests, !ValidateRenderTargetClearValue(NonFiniteClearColor).has_value());

		SamplerDesc SamplerDescription;
		PF_CHECK(Tests, ValidateSamplerDescription(SamplerDescription).has_value());
		auto UnsupportedFilter = SamplerDescription;
		UnsupportedFilter.Minification = static_cast<SamplerFilter>(0xff);
		PF_CHECK(Tests, !ValidateSamplerDescription(UnsupportedFilter).has_value());
		auto UnsupportedAddressMode = SamplerDescription;
		UnsupportedAddressMode.AddressV = static_cast<SamplerAddressMode>(0xff);
		PF_CHECK(Tests, !ValidateSamplerDescription(UnsupportedAddressMode).has_value());
	}

	void TestTextureSamplerAndBufferBindingValidation(TestRunner& Tests)
	{
		using namespace PulseForge;
		BindingLayoutDesc LayoutDescription;
		LayoutDescription.Visibility = ShaderVisibility::Fragment;
		LayoutDescription.Items = {
			{ BindingResourceType::Texture2D, 0 },
			{ BindingResourceType::Sampler, 0 },
			{ BindingResourceType::ConstantBuffer, 0 }
		};
		LayoutDescription.DebugName = "Test texture/sampler/constants";
		PF_CHECK(Tests, ValidateBindingLayout(LayoutDescription).has_value());

		auto DuplicateTextureSlot = LayoutDescription;
		DuplicateTextureSlot.Items.push_back({ BindingResourceType::Texture2D, 0 });
		PF_CHECK(Tests, !ValidateBindingLayout(DuplicateTextureSlot).has_value());

		BindingLayoutHandle Layout = std::make_shared<TestBindingLayout>(LayoutDescription);
		TextureDesc TextureDescription;
		TextureDescription.Width = 1;
		TextureDescription.Height = 1;
		TextureDescription.DebugName = "Test texture";
		TestTexture FakeTexture(TextureDescription);
		TextureDesc DepthTextureDescription;
		DepthTextureDescription.Width = 1;
		DepthTextureDescription.Height = 1;
		DepthTextureDescription.Format = TextureFormat::Depth32Float;
		DepthTextureDescription.Usage = TextureUsage::DepthStencilAttachment;
		TestTexture FakeDepthTexture(DepthTextureDescription);
		DepthTextureDescription.Usage = TextureUsage::DepthStencilAttachment | TextureUsage::ShaderResource;
		TestTexture FakeSampleableDepthTexture(DepthTextureDescription);
		TestSampler Sampler;
		TestBuffer ConstantBuffer(BufferDesc{ 16, BufferUsage::Constant, "Test constants" });

		BindingSetDesc SetDescription;
		SetDescription.Layout = Layout;
		SetDescription.Textures.push_back({ 0, std::cref(static_cast<const PulseForge::Texture&>(FakeTexture)) });
		SetDescription.Samplers.push_back({ 0, std::cref(static_cast<const PulseForge::Sampler&>(Sampler)) });
		SetDescription.Buffers.push_back({ 0, std::cref(static_cast<const Buffer&>(ConstantBuffer)) });
		PF_CHECK(Tests, ValidateBindingSet(SetDescription).has_value());

		auto CubeLayoutDescription = LayoutDescription;
		CubeLayoutDescription.Items[0].Type = BindingResourceType::TextureCube;
		BindingLayoutHandle CubeLayout = std::make_shared<TestBindingLayout>(CubeLayoutDescription);
		TextureDesc CubeTextureDescription = TextureDescription;
		CubeTextureDescription.Dimension = TextureDimension::TextureCube;
		TestTexture FakeCubeTexture(CubeTextureDescription);
		auto CubeBindingDescription = SetDescription;
		CubeBindingDescription.Layout = CubeLayout;
		CubeBindingDescription.Textures[0].Resource = std::cref(static_cast<const Texture&>(FakeCubeTexture));
		CubeBindingDescription.Textures[0].Type = BindingResourceType::TextureCube;
		PF_CHECK(Tests, ValidateBindingSet(CubeBindingDescription).has_value());
		auto WrongCubeDimension = CubeBindingDescription;
		WrongCubeDimension.Textures[0].Resource = std::cref(static_cast<const Texture&>(FakeTexture));
		PF_CHECK(Tests, !ValidateBindingSet(WrongCubeDimension).has_value());
		auto Wrong2DDimension = SetDescription;
		Wrong2DDimension.Textures[0].Resource = std::cref(static_cast<const Texture&>(FakeCubeTexture));
		PF_CHECK(Tests, !ValidateBindingSet(Wrong2DDimension).has_value());

		auto DepthTextureBinding = SetDescription;
		DepthTextureBinding.Textures[0].Resource = std::cref(static_cast<const Texture&>(FakeDepthTexture));
		PF_CHECK(Tests, !ValidateBindingSet(DepthTextureBinding).has_value());
		DepthTextureBinding.Textures[0].Resource = std::cref(static_cast<const Texture&>(FakeSampleableDepthTexture));
		PF_CHECK(Tests, ValidateBindingSet(DepthTextureBinding).has_value());
		DepthTextureBinding.Textures[0].Resource = std::cref(static_cast<const Texture&>(FakeSampleableDepthTexture));
		DepthTextureBinding.Textures[0].Type = BindingResourceType::TextureCube;
		PF_CHECK(Tests, !ValidateBindingSet(DepthTextureBinding).has_value());

		auto MissingTexture = SetDescription;
		MissingTexture.Textures.clear();
		PF_CHECK(Tests, !ValidateBindingSet(MissingTexture).has_value());

		auto WrongSamplerSlot = SetDescription;
		WrongSamplerSlot.Samplers[0].Slot = 1;
		PF_CHECK(Tests, !ValidateBindingSet(WrongSamplerSlot).has_value());

		auto DuplicateSamplerSlot = SetDescription;
		DuplicateSamplerSlot.Samplers.push_back(DuplicateSamplerSlot.Samplers[0]);
		PF_CHECK(Tests, !ValidateBindingSet(DuplicateSamplerSlot).has_value());
	}

	void TestVulkanQueueFamilySelection(TestRunner& Tests)
	{
		using namespace PulseForge::VulkanSupport;
		const std::vector<QueueFamily> SameFamily = {
			{ 0, 0, true, true },
			{ 1, 1, true, true }
		};
		auto Same = ChooseQueueFamilies(SameFamily);
		PF_CHECK(Tests, Same.has_value());
		PF_CHECK(Tests, Same->GraphicsIndex == 1 && Same->PresentIndex == 1);

		const std::vector<QueueFamily> SeparateFamilies = {
			{ 3, 1, true, false },
			{ 5, 1, false, true }
		};
		auto Separate = ChooseQueueFamilies(SeparateFamilies);
		PF_CHECK(Tests, Separate.has_value());
		PF_CHECK(Tests, Separate->GraphicsIndex == 3 && Separate->PresentIndex == 5);

		const std::vector<QueueFamily> MissingPresent = { { 0, 1, true, false } };
		PF_CHECK(Tests, !ChooseQueueFamilies(MissingPresent).has_value());
	}

	void TestVulkanRequiredDeviceFeatures(TestRunner& Tests)
	{
		using namespace PulseForge::VulkanSupport;
		PF_CHECK(Tests, SupportsRequiredDeviceFeatures({ true, true, true, true }));
		PF_CHECK(Tests, !SupportsRequiredDeviceFeatures({ false, true, true, true }));
		PF_CHECK(Tests, !SupportsRequiredDeviceFeatures({ true, false, true, true }));
		PF_CHECK(Tests, !SupportsRequiredDeviceFeatures({ true, true, false, true }));
		PF_CHECK(Tests, !SupportsRequiredDeviceFeatures({ true, true, true, false }));
	}

	void TestVulkanSwapchainRetirementPolicy(TestRunner& Tests)
	{
		using namespace PulseForge::VulkanSupport;
		PF_CHECK(Tests, ResolveSwapchainFrameStartAction(false, true, false) == SwapchainFrameStartAction::Skip);
		PF_CHECK(Tests, ResolveSwapchainFrameStartAction(false, true, true) == SwapchainFrameStartAction::Recreate);
		PF_CHECK(Tests, ResolveSwapchainFrameStartAction(true, true, false) == SwapchainFrameStartAction::PresentCurrent);
		PF_CHECK(Tests, ResolveSwapchainFrameStartAction(true, true, true) == SwapchainFrameStartAction::Recreate);
		PF_CHECK(Tests, ResolveSwapchainFrameStartAction(true, false, false) == SwapchainFrameStartAction::PresentCurrent);
		PF_CHECK(Tests, CanCreateSwapchainGeneration(true, true, MaximumDeferredSwapchainGenerations));
		PF_CHECK(Tests, CanCreateSwapchainGeneration(false, true, 0));
		PF_CHECK(Tests, CanCreateSwapchainGeneration(false, true, MaximumDeferredSwapchainGenerations - 1));
		PF_CHECK(Tests, !CanCreateSwapchainGeneration(false, true, MaximumDeferredSwapchainGenerations));
		PF_CHECK(Tests, CanCreateSwapchainGeneration(false, false, MaximumDeferredSwapchainGenerations));
		PF_CHECK(Tests, CanCreateSwapchainGeneration(false, true, MaximumDeferredSwapchainGenerations, true));
		PF_CHECK(Tests, !CanCreateSwapchainGeneration(false, true,
			MaximumDeferredSwapchainGenerations + MaximumOutOfDateRecoveryGenerations, true));
		PF_CHECK(Tests, ResolveSwapchainFrameStartAction(true, true, false, false) ==
			SwapchainFrameStartAction::Skip);
		PF_CHECK(Tests, ResolveSwapchainFrameStartAction(true, true, false, true) ==
			SwapchainFrameStartAction::PresentCurrent);
		PF_CHECK(Tests, !ShouldRetireDeferredSwapchains(false, false, 2, 2, true));
		PF_CHECK(Tests, !ShouldRetireDeferredSwapchains(false, true, 2, 1, true));
		PF_CHECK(Tests, !ShouldRetireDeferredSwapchains(false, true, 2, 2, false));
		PF_CHECK(Tests, !ShouldRetireDeferredSwapchains(true, true, 2, 2, true));
		PF_CHECK(Tests, ShouldRetireDeferredSwapchains(false, true, 2, 2, true));
	}

	void TestVulkanFormatUsageCapabilities(TestRunner& Tests)
	{
		using namespace PulseForge::VulkanSupport;
		const FormatFeatureSupport AllFeatures{ true, true, true };
		PF_CHECK(Tests, SupportsFormatUsage(AllFeatures, true, true, false));
		PF_CHECK(Tests, SupportsFormatUsage(AllFeatures, true, false, false));
		PF_CHECK(Tests, SupportsFormatUsage(AllFeatures, false, true, false));
		PF_CHECK(Tests, SupportsFormatUsage(AllFeatures, true, true, true));
		PF_CHECK(Tests, SupportsFormatUsage({}, false, false, false));
		PF_CHECK(Tests, !SupportsFormatUsage({ false, true, true }, true, true, false));
		PF_CHECK(Tests, !SupportsFormatUsage({ true, false, true }, true, true, false));
		PF_CHECK(Tests, !SupportsFormatUsage({ true, true, false }, true, true, true));
		PF_CHECK(Tests, !SupportsFormatUsage({ true, true, false }, false, false, true));
		PF_CHECK(Tests, !SupportsFormatUsage({ false, false, true }, false, false, true));
	}

	void TestVulkanSurfaceAndPresentationSelection(TestRunner& Tests)
	{
		using namespace PulseForge::VulkanSupport;
		const std::vector<SurfaceFormat> Formats = { { 10, 1 }, { 20, 3 } };
		auto Preferred = ChooseSurfaceFormat(Formats, 20, 3, 0);
		PF_CHECK(Tests, Preferred.has_value());
		PF_CHECK(Tests, Preferred->Format == 20 && Preferred->ColorSpace == 3);

		const std::vector<SurfaceFormat> UndefinedFormat = { { 0, 5 } };
		auto ChosenForUndefined = ChooseSurfaceFormat(UndefinedFormat, 20, 3, 0);
		PF_CHECK(Tests, ChosenForUndefined.has_value());
		PF_CHECK(Tests, ChosenForUndefined->Format == 20 && ChosenForUndefined->ColorSpace == 5);

		const std::vector<PresentMode> Modes = {
			PresentMode::Immediate, PresentMode::Mailbox, PresentMode::Fifo
		};
		PF_CHECK(Tests, ChoosePresentMode(Modes, true) == PresentMode::Fifo);
		PF_CHECK(Tests, ChoosePresentMode(Modes, false) == PresentMode::Mailbox);
		const std::vector<PresentMode> NoMailbox = { PresentMode::Immediate, PresentMode::Fifo };
		PF_CHECK(Tests, ChoosePresentMode(NoMailbox, false) == PresentMode::Immediate);
	}

	void TestVulkanExtentAndImageCountSelection(TestRunner& Tests)
	{
		using namespace PulseForge::VulkanSupport;
		const auto Clamped = ChooseExtent({ 2400, 200 }, {}, false, { 640, 480 }, { 1920, 1080 });
		PF_CHECK(Tests, Clamped.Width == 1920 && Clamped.Height == 480);

		const auto Fixed = ChooseExtent({ 800, 600 }, { 1600, 900 }, true, {}, {});
		PF_CHECK(Tests, Fixed.Width == 1600 && Fixed.Height == 900);

		const auto Minimized = ChooseExtent({ 0, 0 }, {}, false, { 640, 480 }, { 1920, 1080 });
		PF_CHECK(Tests, Minimized.Width == 0 && Minimized.Height == 0);
		PF_CHECK(Tests, ChooseImageCount(2, 0) == 3);
		PF_CHECK(Tests, ChooseImageCount(2, 2) == 2);
		PF_CHECK(Tests, ChooseImageCount(3, 5) == 4);
	}
}

void RunRendererTests(TestRunner& Tests)
{
	TestSceneSerializerClone(Tests);
	TestPrefabSerialization(Tests);
	TestPrefabAssetsByUUID(Tests);
	TestScreenSpaceAmbientOcclusionMath(Tests);
	TestEnvironmentLightingProcessing(Tests);
	TestGraphicsPipelineAndDrawValidation(Tests);
	TestToneMappingMathAndOutputEncoding(Tests);
	TestConstantBufferBindingValidation(Tests);
	TestTextureAndSamplerValidation(Tests);
	TestTextureSamplerAndBufferBindingValidation(Tests);
	TestVulkanQueueFamilySelection(Tests);
	TestVulkanRequiredDeviceFeatures(Tests);
	TestVulkanSwapchainRetirementPolicy(Tests);
	TestVulkanFormatUsageCapabilities(Tests);
	TestVulkanSurfaceAndPresentationSelection(Tests);
	TestVulkanExtentAndImageCountSelection(Tests);
}
