#include "TestAssetSupport.h"

namespace
{
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
		if (!Root || !Child) return;

		TransformComponent RootTransform;
		RootTransform.Translation = { 2.0f, 3.0f, 4.0f };
		PF_CHECK(Tests, Root->SetTransform(RootTransform).has_value());
		PF_CHECK(Tests, Child->SetParent(*Root).has_value());
		PF_CHECK(Tests, Child->SetMeshRenderer(MeshRendererComponent{ MeshIdentifier, MaterialIdentifier }).has_value());

		auto Clone = SceneSerializer::Clone(Source);
		PF_CHECK(Tests, Clone.has_value() && Clone->get() != &Source);
		if (!Clone) return;

		PF_CHECK(Tests, (*Clone)->GetEntityCount() == Source.GetEntityCount());
		auto ClonedRoot = (*Clone)->FindEntity(RootIdentifier);
		auto ClonedChild = (*Clone)->FindEntity(ChildIdentifier);
		PF_CHECK(Tests, ClonedRoot.has_value() && ClonedChild.has_value());
		if (!ClonedRoot || !ClonedChild) return;

		const auto ClonedTransform = ClonedRoot->GetTransform();
		const auto ClonedRenderer = ClonedChild->GetMeshRenderer();
		const auto ClonedParent = ClonedChild->GetParent();
		PF_CHECK(Tests, ClonedTransform && glm::all(glm::equal(ClonedTransform->Translation, RootTransform.Translation)));
		PF_CHECK(Tests, ClonedRenderer && ClonedRenderer->has_value() && ClonedRenderer->value().MeshAsset == MeshIdentifier &&
							ClonedRenderer->value().MaterialAsset == MaterialIdentifier);
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
		if (!ExternalParent || !Root || !Child || !Grandchild) return;

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
		if (!PrefabData) return;
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
		for (size_t PrimaryFlag = PrimaryRolePrefab.find("\"primary\": false"); PrimaryFlag != std::string::npos;
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
		if (!FirstInstance) return;
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
		PF_CHECK(Tests, FirstLight && FirstLight->has_value() && FirstLight->value().Intensity == PrefabLight.Intensity);
		PF_CHECK(Tests, FirstEnvironment && FirstEnvironment->has_value() &&
							FirstEnvironment->value().HdrImage == PrefabEnvironment.HdrImage &&
							FirstEnvironment->value().Intensity == PrefabEnvironment.Intensity);
		PF_CHECK(Tests, FirstPointLight && FirstPointLight->has_value() && FirstPointLight->value().Range == 9.0f &&
							FirstPointLight->value().CastShadows && FirstPointLight->value().ShadowBias == PrefabPoint.ShadowBias &&
							FirstPointLight->value().ShadowNormalBias == PrefabPoint.ShadowNormalBias &&
							FirstPointLight->value().ShadowSoftness == PrefabPoint.ShadowSoftness);
		PF_CHECK(Tests, FirstAudioListener && FirstAudioListener->has_value() && !FirstAudioListener->value().IsPrimary);
		const auto FirstChildren = FirstInstance->GetChildren();
		PF_CHECK(Tests, FirstChildren && FirstChildren->size() == 1);
		if (!FirstChildren || FirstChildren->size() != 1) return;
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
		PF_CHECK(Tests, FirstChildScript && FirstChildScript->has_value() && FirstChildScript->value().ScriptAsset == ScriptAssetID &&
							!FirstChildScript->value().Enabled);
		PF_CHECK(Tests, FirstGrandchildren && FirstGrandchildren->size() == 1);
		if (FirstGrandchildren && FirstGrandchildren->size() == 1)
		{
			const auto FirstGrandchildSpot = FirstGrandchildren->front().GetSpotLight();
			PF_CHECK(Tests, FirstGrandchildSpot && FirstGrandchildSpot->has_value() &&
								FirstGrandchildSpot->value().OuterConeAngleDegrees == 22.0f && FirstGrandchildSpot->value().CastShadows &&
								FirstGrandchildSpot->value().ShadowBias == 0.003f);
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
				RootIdentifierPosition, Root->GetUUID().ToString().size(), UUID{ 0x7400000000000000ull, 7 }.ToString());
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
		PF_CHECK(Tests, !UnsupportedPrefabVersion && UnsupportedPrefabVersion.error().Code == PrefabErrorCode::UnsupportedVersion);
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

	void TestPrefabSerializationPreservesPhysicsComponents(TestRunner& Tests)
	{
		using namespace PulseForge;
		Scene Source;
		const auto Body = Source.CreateEntity("Physics body");
		PF_CHECK(Tests, Body.has_value());
		if (!Body) return;

		const RigidbodyComponent Rigidbody{ RigidbodyMotionType::Dynamic, 3.0f, 0.6f, 0.15f, false };
		const BoxColliderComponent Collider{ { 0.75f, 0.5f, 0.25f } };
		PF_CHECK(Tests, Body->SetRigidbody(Rigidbody).has_value());
		PF_CHECK(Tests, Body->SetBoxCollider(Collider).has_value());

		const auto PrefabData = PrefabSerializer::Serialize(Source, *Body);
		PF_CHECK(Tests, PrefabData.has_value());
		if (!PrefabData) return;
		PF_CHECK(Tests, PrefabData->find("\"version\": 3") != std::string::npos);

		Scene Destination;
		const auto Instance = PrefabSerializer::Instantiate(*PrefabData, Destination);
		PF_CHECK(Tests, Instance.has_value());
		if (!Instance) return;
		const auto InstanceRigidbody = Instance->GetRigidbody();
		const auto InstanceCollider = Instance->GetBoxCollider();
		PF_CHECK(Tests, InstanceRigidbody && InstanceRigidbody->has_value() && InstanceRigidbody->value().Mass == 3.0f);
		PF_CHECK(Tests, InstanceCollider && InstanceCollider->has_value() && InstanceCollider->value().HalfExtents == Collider.HalfExtents);
	}

	void TestPrefabAssetsByUUID(TestRunner& Tests)
	{
		using namespace PulseForge;
		std::error_code FileError;
		const std::filesystem::path TemporaryDirectory = std::filesystem::temp_directory_path(FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError) return;

		const auto ProjectIdentifier = UUID::Generate();
		PF_CHECK(Tests, ProjectIdentifier.has_value());
		if (!ProjectIdentifier) return;

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
		if (FileError) return;

		const auto RootID = UUID::Parse("15261902-2c12-4a92-9e99-0e355a16af4e");
		const auto ChildID = UUID::Parse("df753d9d-1024-40f1-980f-5ca589fd87d8");
		const auto MeshAssetID = UUID::Parse("d3ce3630-b87a-4dab-b8e0-59c1f258ae78");
		PF_CHECK(Tests, RootID && ChildID && MeshAssetID);
		if (!RootID || !ChildID || !MeshAssetID) return;

		Scene Source;
		const auto Root = Source.CreateEntityWithUUID(*RootID, "Prefab Root");
		const auto Child = Source.CreateEntityWithUUID(*ChildID, "Prefab Child");
		PF_CHECK(Tests, Root && Child);
		if (!Root || !Child) return;
		PF_CHECK(Tests, Child->SetParent(*Root).has_value());
		PF_CHECK(Tests, Child->SetMeshRenderer(MeshRendererComponent{ *MeshAssetID }).has_value());

		const std::filesystem::path SourcePath = PrefabDirectory / "crate.prefab";
		PF_CHECK(Tests, PrefabSerializer::SaveToFile(Source, *Root, SourcePath).has_value());
		const auto Metadata = AssetMetadataSerializer::CreateForNewAsset(SourcePath);
		PF_CHECK(Tests, Metadata.has_value());
		if (!Metadata) return;

		AssetRegistry Registry;
		PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());
		Scene Destination;
		const auto Existing = Destination.CreateEntity("Existing entity");
		PF_CHECK(Tests, Existing.has_value());
		const auto FirstInstance = PrefabAssetService::Instantiate(Metadata->ID, ProjectRoot, Registry, Destination);
		PF_CHECK(Tests, FirstInstance.has_value());
		PF_CHECK(Tests, FirstInstance && FirstInstance->GetUUID() != *RootID);
		PF_CHECK(Tests, Destination.GetEntityCount() == 3);
		if (!FirstInstance) return;
		const auto FirstChildren = FirstInstance->GetChildren();
		PF_CHECK(Tests, FirstChildren && FirstChildren->size() == 1);
		if (!FirstChildren || FirstChildren->size() != 1) return;
		const auto ChildRenderer = FirstChildren->front().GetMeshRenderer();
		PF_CHECK(Tests, ChildRenderer && ChildRenderer->has_value());
		PF_CHECK(Tests, ChildRenderer && ChildRenderer->has_value() && ChildRenderer->value().MeshAsset == *MeshAssetID);

		const auto MoveResult = AssetOperations::Move(Registry, ProjectRoot, Metadata->ID, "Assets/Prefabs/renamed.prefab");
		PF_CHECK(Tests, MoveResult && MoveResult->ID == Metadata->ID);
		PF_CHECK(Tests, PrefabAssetService::Instantiate(Metadata->ID, ProjectRoot, Registry, Destination).has_value());

		auto RootTag = Root->GetTag();
		PF_CHECK(Tests, RootTag.has_value());
		if (!RootTag) return;
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

		const auto CreatedPrefab = PrefabAssetService::Create(Registry, ProjectRoot, "Assets/Prefabs/generated.prefab", Source, *Root);
		PF_CHECK(Tests, CreatedPrefab.has_value());
		PF_CHECK(Tests,
			CreatedPrefab && CreatedPrefab->ID != Metadata->ID && CreatedPrefab->ProjectRelativePath == "Assets/Prefabs/generated.prefab");
		if (CreatedPrefab)
		{
			const auto CreatedMetadata = AssetMetadataSerializer::LoadFromFile(ProjectRoot / "Assets/Prefabs/generated.prefab.meta");
			PF_CHECK(Tests, CreatedMetadata && CreatedMetadata->ID == CreatedPrefab->ID);
			PF_CHECK(Tests, Registry.Find(CreatedPrefab->ID).has_value());

			Scene CreatedPrefabScene;
			const auto CreatedRoot = PrefabAssetService::Instantiate(CreatedPrefab->ID, ProjectRoot, Registry, CreatedPrefabScene);
			PF_CHECK(Tests, CreatedRoot && CreatedRoot->GetUUID() != Root->GetUUID());
			PF_CHECK(Tests, CreatedPrefabScene.GetEntityCount() == 2);
			if (CreatedRoot)
			{
				const auto CreatedChildren = CreatedRoot->GetChildren();
				PF_CHECK(Tests, CreatedChildren && CreatedChildren->size() == 1);
			}

			const auto DuplicateCreation =
				PrefabAssetService::Create(Registry, ProjectRoot, "Assets/Prefabs/generated.prefab", Source, *Root);
			PF_CHECK(Tests, !DuplicateCreation && DuplicateCreation.error().Code == PrefabAssetErrorCode::AssetOperationFailed);
			PF_CHECK(Tests, Registry.Find(CreatedPrefab->ID).has_value());
		}
		const auto InvalidPrefabCreation =
			PrefabAssetService::Create(Registry, ProjectRoot, "Assets/Prefabs/generated.json", Source, *Root);
		PF_CHECK(Tests, !InvalidPrefabCreation && InvalidPrefabCreation.error().Code == PrefabAssetErrorCode::UnsupportedAssetType);

		const auto Duplicate = AssetOperations::Duplicate(Registry, ProjectRoot, Metadata->ID, "Assets/Prefabs/copy.prefab");
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

} // namespace

void RunSceneAssetTests(TestRunner& Tests)
{
	TestSceneSerializerClone(Tests);
	TestPrefabSerialization(Tests);
	TestPrefabSerializationPreservesPhysicsComponents(Tests);
	TestPrefabAssetsByUUID(Tests);
}
