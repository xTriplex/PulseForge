#include "TestEngineSupport.h"

namespace
{
	void TestSceneSerializationRoundTrip(TestRunner& Tests)
	{
		using namespace PulseForge;
		Scene Source;
		const UUID RootId{ 0x2000000000000000ull, 2 };
		const UUID ChildId{ 0x1000000000000000ull, 1 };
		const AssetID MeshAssetIdentifier{ 0x5000000000000000ull, 5 };
		const AssetID MaterialAssetIdentifier{ 0x5100000000000000ull, 51 };
		const AssetID AudioAssetIdentifier{ 0x5200000000000000ull, 52 };
		const AssetID ScriptAssetIdentifier{ 0x5300000000000000ull, 53 };
		auto ChildResult = Source.CreateEntityWithUUID(ChildId, "Child \"one\"");
		auto RootResult = Source.CreateEntityWithUUID(RootId, "Root");
		PF_CHECK(Tests, ChildResult.has_value() && RootResult.has_value());
		if (!ChildResult || !RootResult)
			return;

		Entity Child = *ChildResult;
		Entity Root = *RootResult;
		TransformComponent RootTransform;
		RootTransform.Translation = { 3.0f, 0.0f, 0.0f };
		TransformComponent ChildTransform;
		ChildTransform.Translation = { 4.0f, 2.0f, 1.0f };
		ChildTransform.Rotation = glm::quat(0.9238795f, 0.0f, 0.3826834f, 0.0f);
		ChildTransform.Scale = { 2.0f, 2.0f, 2.0f };
		PF_CHECK(Tests, Root.SetTransform(RootTransform).has_value());
		PF_CHECK(Tests, Child.SetTransform(ChildTransform).has_value());
		PF_CHECK(Tests, Child.SetParent(Root).has_value());
		CameraComponent SourceCamera;
		SourceCamera.VerticalFieldOfViewRadians = 0.9f;
		SourceCamera.NearClipPlane = 0.25f;
		SourceCamera.FarClipPlane = 80.0f;
		SourceCamera.IsPrimary = true;
		PF_CHECK(Tests, SourceCamera.Validate().has_value());
		const auto SourceProjection = SourceCamera.GetProjectionMatrix(16.0f / 9.0f);
		PF_CHECK(Tests, SourceProjection.has_value());
		PF_CHECK(Tests, SourceProjection && (*SourceProjection)[1][1] > 0.0f);
		const auto InvalidAspectProjection = SourceCamera.GetProjectionMatrix(0.0f);
		PF_CHECK(Tests, !InvalidAspectProjection.has_value());
		const auto GLMAssertAspectProjection = SourceCamera.GetProjectionMatrix(std::numeric_limits<float>::epsilon());
		PF_CHECK(Tests, !GLMAssertAspectProjection.has_value());
		CameraComponent OverflowingProjectionCamera = SourceCamera;
		OverflowingProjectionCamera.NearClipPlane = 2.0f;
		OverflowingProjectionCamera.FarClipPlane = std::numeric_limits<float>::max();
		const auto OverflowingProjection = OverflowingProjectionCamera.GetProjectionMatrix(16.0f / 9.0f);
		PF_CHECK(Tests, !OverflowingProjection.has_value());
		PF_CHECK(Tests, !OverflowingProjection && OverflowingProjection.error().Code == CameraErrorCode::NonFiniteProjection);
		CameraComponent InvalidCamera = SourceCamera;
		InvalidCamera.VerticalFieldOfViewRadians = 0.0f;
		const auto InvalidCameraSet = Child.SetCamera(InvalidCamera);
		PF_CHECK(Tests, !InvalidCameraSet.has_value());
		PF_CHECK(Tests, !InvalidCameraSet && InvalidCameraSet.error().Code == SceneErrorCode::InvalidCamera);
		const auto CameraBeforeSet = Child.GetCamera();
		PF_CHECK(Tests, CameraBeforeSet && !CameraBeforeSet->has_value());
		PF_CHECK(Tests, Child.SetCamera(SourceCamera).has_value());
		DirectionalLightComponent SourceLight{ glm::vec3(0.75f, 0.8f, 1.0f), 3.0f };
		SourceLight.CastShadows = false;
		SourceLight.ShadowDistance = 42.0f;
		SourceLight.ShadowBias = 2.0f;
		SourceLight.ShadowNormalBias = 0.04f;
		SourceLight.ShadowSoftness = 3.0f;
		PF_CHECK(Tests, SourceLight.Validate().has_value());
		PF_CHECK(Tests, Child.SetDirectionalLight(SourceLight).has_value());
		PointLightComponent SourcePointLight{ glm::vec3(0.2f, 0.7f, 0.4f), 5.5f, 18.0f };
		SourcePointLight.CastShadows = true;
		SourcePointLight.ShadowBias = 0.003f;
		SourcePointLight.ShadowNormalBias = 0.08f;
		SourcePointLight.ShadowSoftness = 2.25f;
		SpotLightComponent SourceSpotLight{ glm::vec3(0.5f, 0.6f, 0.9f), 12.0f, 24.0f, 12.0f, 30.0f };
		SourceSpotLight.CastShadows = true;
		SourceSpotLight.ShadowBias = 0.0025f;
		SourceSpotLight.ShadowNormalBias = 0.05f;
		SourceSpotLight.ShadowSoftness = 2.0f;
		PF_CHECK(Tests, Child.SetPointLight(SourcePointLight).has_value());
		PF_CHECK(Tests, Child.SetSpotLight(SourceSpotLight).has_value());
		const EnvironmentLightComponent SourceEnvironment{ AssetID{ 0x7400000000000000ull, 74 }, 1.25f };
		PF_CHECK(Tests, SourceEnvironment.Validate().has_value());
		const EnvironmentLightComponent MissingEnvironmentAsset{ AssetID{}, 1.0f };
		const EnvironmentLightComponent NegativeEnvironmentIntensity{ SourceEnvironment.HdrImage, -1.0f };
		const EnvironmentLightComponent InfiniteEnvironmentIntensity{
			SourceEnvironment.HdrImage, std::numeric_limits<float>::infinity() };
		PF_CHECK(Tests, !MissingEnvironmentAsset.Validate().has_value());
		PF_CHECK(Tests, !NegativeEnvironmentIntensity.Validate().has_value());
		PF_CHECK(Tests, !InfiniteEnvironmentIntensity.Validate().has_value());
		PF_CHECK(Tests, Child.SetEnvironmentLight(SourceEnvironment).has_value());
		PF_CHECK(Tests, Child.SetMeshRenderer(MeshRendererComponent{ MeshAssetIdentifier, MaterialAssetIdentifier }).has_value());
		PF_CHECK(Tests, !Child.SetMeshRenderer(MeshRendererComponent{ MeshAssetIdentifier, AssetID{} }).has_value());
		const AudioSourceComponent SourceAudio{ AudioAssetIdentifier, 0.35f, true, false, true };
		PF_CHECK(Tests, Child.SetAudioSource(SourceAudio).has_value());
		PF_CHECK(Tests, Child.SetScript(ScriptComponent{ ScriptAssetIdentifier, false }).has_value());
		const auto InvalidScriptSet = Child.SetScript(ScriptComponent{});
		PF_CHECK(Tests, !InvalidScriptSet && InvalidScriptSet.error().Code == SceneErrorCode::InvalidScriptComponent);
		PF_CHECK(Tests, Root.SetAudioListener(AudioListenerComponent{ true }).has_value());
		AudioSourceComponent InvalidAudio = SourceAudio;
		InvalidAudio.AudioAsset = AssetID{};
		const auto InvalidAudioSet = Child.SetAudioSource(InvalidAudio);
		PF_CHECK(Tests, !InvalidAudioSet && InvalidAudioSet.error().Code == SceneErrorCode::InvalidAudioComponent);
		const auto RootCamera = Root.GetCamera();
		PF_CHECK(Tests, RootCamera.has_value() && !RootCamera->has_value());

		const auto Serialized = SceneSerializer::Serialize(Source);
		PF_CHECK(Tests, Serialized.has_value());
		if (!Serialized)
			return;
		PF_CHECK(Tests, Serialized->find("\"format\": \"PulseForgeScene\"") != std::string::npos);
		PF_CHECK(Tests, Serialized->find("\"version\": 7") != std::string::npos);
		PF_CHECK(Tests, Serialized->find("\"primary\": true") != std::string::npos);
		PF_CHECK(Tests, Serialized->find("\"directionalLight\"") != std::string::npos);
		PF_CHECK(Tests, Serialized->find("\"castShadows\": false") != std::string::npos);
		PF_CHECK(Tests, Serialized->find("\"shadowDistance\": 42.0") != std::string::npos);
		PF_CHECK(Tests, Serialized->find("\"environmentLight\"") != std::string::npos);
		PF_CHECK(Tests, Serialized->find("\"pointLight\"") != std::string::npos);
		PF_CHECK(Tests, Serialized->find("\"shadowSoftness\": 2.25") != std::string::npos);
		PF_CHECK(Tests, Serialized->find("\"spotLight\"") != std::string::npos);
		PF_CHECK(Tests, Serialized->find("\"meshAsset\": \"" + MeshAssetIdentifier.ToString() + "\"") != std::string::npos);
		PF_CHECK(Tests, Serialized->find("\"materialAsset\": \"" + MaterialAssetIdentifier.ToString() + "\"") != std::string::npos);
		PF_CHECK(Tests, Serialized->find("\"asset\": \"" + AudioAssetIdentifier.ToString() + "\"") != std::string::npos);
		PF_CHECK(Tests, Serialized->find("\"playOnStart\": false") != std::string::npos);
		PF_CHECK(Tests, Serialized->find("\"audioListener\"") != std::string::npos);
		PF_CHECK(Tests, Serialized->find("\"script\"") != std::string::npos);
		PF_CHECK(Tests, Serialized->find("\"asset\": \"" + ScriptAssetIdentifier.ToString() + "\"") != std::string::npos);
		PF_CHECK(Tests, Serialized->find("Assets/") == std::string::npos);
		const size_t ChildEntityPosition = Serialized->find("\"uuid\": \"" + ChildId.ToString() + "\"");
		const size_t RootEntityPosition = Serialized->find("\"uuid\": \"" + RootId.ToString() + "\"");
		PF_CHECK(Tests, ChildEntityPosition != std::string::npos && RootEntityPosition != std::string::npos &&
			ChildEntityPosition < RootEntityPosition);

		auto LegacyDocument = nlohmann::json::parse(*Serialized);
		bool FoundLegacyLight = false;
		for (auto& EntityRecord : LegacyDocument["entities"])
		{
			if (EntityRecord.value("uuid", std::string{}) != ChildId.ToString())
				continue;
			FoundLegacyLight = EntityRecord.contains("directionalLight");
			if (!FoundLegacyLight)
				break;
			for (const std::string_view Key : { "castShadows", "shadowDistance", "shadowBias", "shadowNormalBias", "shadowSoftness" })
				EntityRecord["directionalLight"].erase(std::string(Key));
			break;
		}
		PF_CHECK(Tests, FoundLegacyLight);
		const std::string LegacySerialized = LegacyDocument.dump(2) + "\n";
		Scene LegacyDestination;
		const auto LegacyLoaded = SceneSerializer::Deserialize(LegacySerialized, LegacyDestination);
		PF_CHECK(Tests, LegacyLoaded.has_value());
		if (LegacyLoaded)
		{
			const auto LegacyEntity = LegacyDestination.FindEntity(ChildId);
			PF_CHECK(Tests, LegacyEntity.has_value());
			if (LegacyEntity)
			{
				const auto LegacyLight = LegacyEntity->GetDirectionalLight();
				PF_CHECK(Tests, LegacyLight && LegacyLight->has_value() && LegacyLight->value().CastShadows);
				PF_CHECK(Tests, LegacyLight && LegacyLight->has_value() && LegacyLight->value().ShadowDistance == 100.0f);
				PF_CHECK(Tests, LegacyLight && LegacyLight->has_value() && LegacyLight->value().ShadowBias == 1.0f &&
					LegacyLight->value().ShadowNormalBias == 0.025f && LegacyLight->value().ShadowSoftness == 1.5f);
			}
		}

		Scene Destination;
		auto PreviousResult = Destination.CreateEntity("Previous scene");
		PF_CHECK(Tests, PreviousResult.has_value());
		if (!PreviousResult)
			return;
		Entity PreviousEntity = *PreviousResult;
		const auto Loaded = SceneSerializer::Deserialize(*Serialized, Destination);
		PF_CHECK(Tests, Loaded.has_value());
		PF_CHECK(Tests, !PreviousEntity.IsValid());
		PF_CHECK(Tests, Destination.GetEntityCount() == 2);

		const auto LoadedRoot = Destination.FindEntity(RootId);
		const auto LoadedChild = Destination.FindEntity(ChildId);
		PF_CHECK(Tests, LoadedRoot.has_value() && LoadedChild.has_value());
		if (!LoadedRoot || !LoadedChild)
			return;

		const auto LoadedTag = LoadedChild->GetTag();
		const auto LoadedTransform = LoadedChild->GetTransform();
		const auto LoadedCamera = LoadedChild->GetCamera();
		const auto LoadedLight = LoadedChild->GetDirectionalLight();
		const auto LoadedEnvironment = LoadedChild->GetEnvironmentLight();
		const auto LoadedPoint = LoadedChild->GetPointLight();
		const auto LoadedSpot = LoadedChild->GetSpotLight();
		const auto LoadedMeshRenderer = LoadedChild->GetMeshRenderer();
		const auto LoadedAudioSource = LoadedChild->GetAudioSource();
		const auto LoadedAudioListener = LoadedRoot->GetAudioListener();
		const auto LoadedScript = LoadedChild->GetScript();
		const auto LoadedParent = LoadedChild->GetParent();
		PF_CHECK(Tests, LoadedTag && LoadedTag->Name == "Child \"one\"");
		PF_CHECK(Tests, LoadedTransform && glm::all(glm::equal(LoadedTransform->Translation, ChildTransform.Translation)));
		PF_CHECK(Tests, LoadedTransform && glm::all(glm::equal(LoadedTransform->Scale, ChildTransform.Scale)));
		PF_CHECK(Tests, LoadedTransform && glm::abs(glm::length(LoadedTransform->Rotation) - 1.0f) < 0.0001f);
		PF_CHECK(Tests, LoadedCamera && LoadedCamera->has_value());
		PF_CHECK(Tests, LoadedCamera && LoadedCamera->has_value() &&
			glm::abs(LoadedCamera->value().VerticalFieldOfViewRadians - SourceCamera.VerticalFieldOfViewRadians) < 0.0001f);
		PF_CHECK(Tests, LoadedCamera && LoadedCamera->has_value() && LoadedCamera->value().IsPrimary);
		PF_CHECK(Tests, LoadedLight && LoadedLight->has_value() &&
			LoadedLight->value().Intensity == SourceLight.Intensity &&
			glm::all(glm::equal(LoadedLight->value().Color, SourceLight.Color)));
		PF_CHECK(Tests, LoadedLight && LoadedLight->has_value() &&
			!LoadedLight->value().CastShadows && LoadedLight->value().ShadowDistance == 42.0f &&
			LoadedLight->value().ShadowBias == 2.0f && LoadedLight->value().ShadowNormalBias == 0.04f &&
			LoadedLight->value().ShadowSoftness == 3.0f);
		PF_CHECK(Tests, LoadedEnvironment && LoadedEnvironment->has_value() &&
			LoadedEnvironment->value().HdrImage == SourceEnvironment.HdrImage &&
			LoadedEnvironment->value().Intensity == SourceEnvironment.Intensity);
		PF_CHECK(Tests, LoadedPoint && LoadedPoint->has_value() &&
			LoadedPoint->value().Intensity == SourcePointLight.Intensity && LoadedPoint->value().Range == SourcePointLight.Range &&
			glm::all(glm::equal(LoadedPoint->value().Color, SourcePointLight.Color)) &&
			LoadedPoint->value().CastShadows && LoadedPoint->value().ShadowBias == SourcePointLight.ShadowBias &&
			LoadedPoint->value().ShadowNormalBias == SourcePointLight.ShadowNormalBias &&
			LoadedPoint->value().ShadowSoftness == SourcePointLight.ShadowSoftness);
		PF_CHECK(Tests, LoadedSpot && LoadedSpot->has_value() &&
			LoadedSpot->value().InnerConeAngleDegrees == SourceSpotLight.InnerConeAngleDegrees &&
			LoadedSpot->value().OuterConeAngleDegrees == SourceSpotLight.OuterConeAngleDegrees &&
			LoadedSpot->value().Range == SourceSpotLight.Range && LoadedSpot->value().CastShadows &&
			LoadedSpot->value().ShadowBias == SourceSpotLight.ShadowBias &&
			LoadedSpot->value().ShadowNormalBias == SourceSpotLight.ShadowNormalBias &&
			LoadedSpot->value().ShadowSoftness == SourceSpotLight.ShadowSoftness);
		PF_CHECK(Tests, LoadedMeshRenderer && LoadedMeshRenderer->has_value() &&
			LoadedMeshRenderer->value().MeshAsset == MeshAssetIdentifier);
		PF_CHECK(Tests, LoadedMeshRenderer && LoadedMeshRenderer->has_value() &&
			LoadedMeshRenderer->value().MaterialAsset == MaterialAssetIdentifier);
		PF_CHECK(Tests, LoadedAudioSource && LoadedAudioSource->has_value() &&
			LoadedAudioSource->value().AudioAsset == AudioAssetIdentifier);
		PF_CHECK(Tests, LoadedAudioSource && LoadedAudioSource->has_value() &&
			LoadedAudioSource->value().Volume == SourceAudio.Volume && LoadedAudioSource->value().Looping &&
			!LoadedAudioSource->value().PlayOnStart && LoadedAudioSource->value().Spatialized);
		PF_CHECK(Tests, LoadedAudioListener && LoadedAudioListener->has_value() && LoadedAudioListener->value().IsPrimary);
		PF_CHECK(Tests, LoadedScript && LoadedScript->has_value() &&
			LoadedScript->value().ScriptAsset == ScriptAssetIdentifier && !LoadedScript->value().Enabled);
		PF_CHECK(Tests, LoadedParent && LoadedParent->has_value() && **LoadedParent == *LoadedRoot);
		auto LegacySpotDocument = nlohmann::json::parse(*Serialized);
		for (auto& EntityRecord : LegacySpotDocument["entities"])
			if (EntityRecord.contains("spotLight"))
			{
				EntityRecord["spotLight"].erase("castShadows");
				EntityRecord["spotLight"].erase("shadowBias");
				EntityRecord["spotLight"].erase("shadowNormalBias");
				EntityRecord["spotLight"].erase("shadowSoftness");
			}
		Scene LegacySpotDestination;
		const auto LegacySpotLoad = SceneSerializer::Deserialize(LegacySpotDocument.dump(), LegacySpotDestination);
		PF_CHECK(Tests, LegacySpotLoad.has_value());
		if (LegacySpotLoad)
		{
			const auto LegacySpotEntity = LegacySpotDestination.FindEntity(ChildId);
			PF_CHECK(Tests, LegacySpotEntity.has_value());
			if (LegacySpotEntity)
			{
				const auto LegacySpot = LegacySpotEntity->GetSpotLight();
				PF_CHECK(Tests, LegacySpot && LegacySpot->has_value() && !LegacySpot->value().CastShadows &&
					LegacySpot->value().ShadowBias == SpotLightComponent{}.ShadowBias);
			}
		}
		auto InvalidSpotDocument = nlohmann::json::parse(*Serialized);
		for (auto& EntityRecord : InvalidSpotDocument["entities"])
			if (EntityRecord.value("uuid", std::string{}) == ChildId.ToString())
				EntityRecord["spotLight"]["innerConeAngleDegrees"] =
					EntityRecord["spotLight"]["outerConeAngleDegrees"];
		const auto InvalidSpotLoad = SceneSerializer::Deserialize(InvalidSpotDocument.dump(), Destination);
		PF_CHECK(Tests, !InvalidSpotLoad);
		PF_CHECK(Tests, Destination.FindEntity(ChildId).has_value() && Destination.GetEntityCount() == 2);
		const auto SerializedAgain = SceneSerializer::Serialize(Destination);
		PF_CHECK(Tests, SerializedAgain && *SerializedAgain == *Serialized);
		if (LoadedCamera && LoadedCamera->has_value())
		{
			PF_CHECK(Tests, LoadedChild->RemoveCamera().has_value());
			const auto RemovedCamera = LoadedChild->GetCamera();
			PF_CHECK(Tests, RemovedCamera && !RemovedCamera->has_value());
			const auto MissingCameraRemoval = LoadedChild->RemoveCamera();
			PF_CHECK(Tests, !MissingCameraRemoval.has_value());
			PF_CHECK(Tests, !MissingCameraRemoval && MissingCameraRemoval.error().Code == SceneErrorCode::MissingComponent);
		}

		const auto DestinationRootBeforeFailure = Destination.FindEntity(RootId);
		std::string UnsupportedVersion = *Serialized;
		const size_t VersionPosition = UnsupportedVersion.find("\"version\": 7");
		PF_CHECK(Tests, VersionPosition != std::string::npos);
		if (VersionPosition != std::string::npos)
			UnsupportedVersion.replace(VersionPosition, std::string("\"version\": 7").size(), "\"version\": 99");
		const auto UnsupportedVersionResult = SceneSerializer::Deserialize(UnsupportedVersion, Destination);
		PF_CHECK(Tests, !UnsupportedVersionResult.has_value());
		PF_CHECK(Tests, !UnsupportedVersionResult && UnsupportedVersionResult.error().Code == SceneSerializationErrorCode::UnsupportedVersion);
		PF_CHECK(Tests, Destination.FindEntity(RootId) == DestinationRootBeforeFailure);

		std::string InvalidAudioVolume = *Serialized;
		const size_t AudioSourcePosition = InvalidAudioVolume.find("\"audioSource\"");
		const size_t AudioVolumePosition = InvalidAudioVolume.find("\"volume\": ", AudioSourcePosition);
		PF_CHECK(Tests, AudioSourcePosition != std::string::npos && AudioVolumePosition != std::string::npos);
		if (AudioVolumePosition != std::string::npos)
		{
			const size_t AudioVolumeValueStart = AudioVolumePosition + std::string("\"volume\": ").size();
			const size_t AudioVolumeValueEnd = InvalidAudioVolume.find_first_of(",\n", AudioVolumeValueStart);
			PF_CHECK(Tests, AudioVolumeValueEnd != std::string::npos);
			if (AudioVolumeValueEnd != std::string::npos)
				InvalidAudioVolume.replace(AudioVolumeValueStart, AudioVolumeValueEnd - AudioVolumeValueStart, "2.0");
		}
		const auto InvalidAudioVolumeResult = SceneSerializer::Deserialize(InvalidAudioVolume, Destination);
		PF_CHECK(Tests, !InvalidAudioVolumeResult &&
			InvalidAudioVolumeResult.error().Code == SceneSerializationErrorCode::InvalidEntityData);
		PF_CHECK(Tests, Destination.FindEntity(RootId) == DestinationRootBeforeFailure);

		std::string LegacyVersionThree = *Serialized;
		const size_t PrimaryFieldPosition = LegacyVersionThree.find("\"primary\": true");
		PF_CHECK(Tests, PrimaryFieldPosition != std::string::npos);
		if (PrimaryFieldPosition != std::string::npos)
		{
			const size_t PrimaryCommaPosition = LegacyVersionThree.rfind(',', PrimaryFieldPosition);
			const size_t PrimaryLineEnd = LegacyVersionThree.find('\n', PrimaryFieldPosition);
			PF_CHECK(Tests, PrimaryCommaPosition != std::string::npos && PrimaryLineEnd != std::string::npos);
			if (PrimaryCommaPosition != std::string::npos && PrimaryLineEnd != std::string::npos)
				LegacyVersionThree.erase(PrimaryCommaPosition, PrimaryLineEnd - PrimaryCommaPosition);
		}
		const size_t LegacyVersionThreePosition = LegacyVersionThree.find("\"version\": 7");
		PF_CHECK(Tests, LegacyVersionThreePosition != std::string::npos);
		if (LegacyVersionThreePosition != std::string::npos)
			LegacyVersionThree.replace(LegacyVersionThreePosition, std::string("\"version\": 7").size(), "\"version\": 3");
		Scene LegacyVersionThreeDestination;
		const auto LegacyVersionThreeLoad = SceneSerializer::Deserialize(LegacyVersionThree, LegacyVersionThreeDestination);
		PF_CHECK(Tests, LegacyVersionThreeLoad.has_value());
		const auto LegacyCamera = LegacyVersionThreeDestination.FindEntity(ChildId);
		PF_CHECK(Tests, LegacyCamera.has_value());
		if (LegacyCamera)
		{
			const auto LegacyCameraComponent = LegacyCamera->GetCamera();
			PF_CHECK(Tests, LegacyCameraComponent && LegacyCameraComponent->has_value() &&
				!LegacyCameraComponent->value().IsPrimary);
		}

		Scene LegacySource;
		const auto LegacyEntity = LegacySource.CreateEntityWithUUID(UUID{ 0x6000000000000000ull, 6 }, "Version one");
		PF_CHECK(Tests, LegacyEntity.has_value());
		const auto LegacySerializedVersionTwo = SceneSerializer::Serialize(LegacySource);
		PF_CHECK(Tests, LegacySerializedVersionTwo.has_value());
		if (LegacySerializedVersionTwo)
		{
			std::string LegacyVersionOne = *LegacySerializedVersionTwo;
			const size_t LegacyVersionPosition = LegacyVersionOne.find("\"version\": 7");
			PF_CHECK(Tests, LegacyVersionPosition != std::string::npos);
			if (LegacyVersionPosition != std::string::npos)
				LegacyVersionOne.replace(LegacyVersionPosition, std::string("\"version\": 7").size(), "\"version\": 1");
			Scene LegacyDestination;
			const auto LegacyLoad = SceneSerializer::Deserialize(LegacyVersionOne, LegacyDestination);
			PF_CHECK(Tests, LegacyLoad.has_value());
			PF_CHECK(Tests, LegacyDestination.GetEntityCount() == 1);
			const auto LegacyLoadedEntity = LegacyDestination.FindEntity(UUID{ 0x6000000000000000ull, 6 });
			PF_CHECK(Tests, LegacyLoadedEntity.has_value());
			if (LegacyLoadedEntity)
			{
				const auto LegacyMeshRenderer = LegacyLoadedEntity->GetMeshRenderer();
				PF_CHECK(Tests, LegacyMeshRenderer && !LegacyMeshRenderer->has_value());
			}
		}

		std::string LegacyVersionTwo = LegacyVersionThree;
		const std::string MaterialField = "\"materialAsset\": \"" + MaterialAssetIdentifier.ToString() + "\"";
		const size_t MaterialFieldPosition = LegacyVersionTwo.find(MaterialField);
		PF_CHECK(Tests, MaterialFieldPosition != std::string::npos);
		if (MaterialFieldPosition != std::string::npos)
		{
			const size_t CommaPosition = LegacyVersionTwo.rfind(',', MaterialFieldPosition);
			const size_t LineEnd = LegacyVersionTwo.find('\n', MaterialFieldPosition);
			PF_CHECK(Tests, CommaPosition != std::string::npos && LineEnd != std::string::npos);
			if (CommaPosition != std::string::npos && LineEnd != std::string::npos)
				LegacyVersionTwo.erase(CommaPosition, LineEnd - CommaPosition);
		}
		const size_t VersionTwoPosition = LegacyVersionTwo.find("\"version\": 3");
		PF_CHECK(Tests, VersionTwoPosition != std::string::npos);
		if (VersionTwoPosition != std::string::npos)
			LegacyVersionTwo.replace(VersionTwoPosition, std::string("\"version\": 3").size(), "\"version\": 2");
		Scene LegacyVersionTwoDestination;
		const auto LegacyVersionTwoLoad = SceneSerializer::Deserialize(LegacyVersionTwo, LegacyVersionTwoDestination);
		PF_CHECK(Tests, LegacyVersionTwoLoad.has_value());
		const auto LegacyVersionTwoChild = LegacyVersionTwoDestination.FindEntity(ChildId);
		PF_CHECK(Tests, LegacyVersionTwoChild.has_value());
		if (LegacyVersionTwoChild)
		{
			const auto LegacyMeshRenderer = LegacyVersionTwoChild->GetMeshRenderer();
			PF_CHECK(Tests, LegacyMeshRenderer && LegacyMeshRenderer->has_value());
			PF_CHECK(Tests, LegacyMeshRenderer && LegacyMeshRenderer->has_value() &&
				!LegacyMeshRenderer->value().MaterialAsset.has_value());
		}

		std::string InvalidMeshAsset = *Serialized;
		const size_t MeshAssetPosition = InvalidMeshAsset.find(MeshAssetIdentifier.ToString());
		PF_CHECK(Tests, MeshAssetPosition != std::string::npos);
		if (MeshAssetPosition != std::string::npos)
			InvalidMeshAsset.replace(MeshAssetPosition, MeshAssetIdentifier.ToString().size(), "not-a-uuid");
		const auto InvalidMeshAssetLoad = SceneSerializer::Deserialize(InvalidMeshAsset, Destination);
		PF_CHECK(Tests, !InvalidMeshAssetLoad.has_value());
		PF_CHECK(Tests, !InvalidMeshAssetLoad &&
			InvalidMeshAssetLoad.error().Code == SceneSerializationErrorCode::InvalidEntityData);
		PF_CHECK(Tests, Destination.FindEntity(RootId) == DestinationRootBeforeFailure);

		std::string InvalidMaterialAsset = *Serialized;
		const size_t InvalidMaterialPosition = InvalidMaterialAsset.find(MaterialAssetIdentifier.ToString());
		PF_CHECK(Tests, InvalidMaterialPosition != std::string::npos);
		if (InvalidMaterialPosition != std::string::npos)
			InvalidMaterialAsset.replace(InvalidMaterialPosition, MaterialAssetIdentifier.ToString().size(), "not-a-uuid");
		const auto InvalidMaterialLoad = SceneSerializer::Deserialize(InvalidMaterialAsset, Destination);
		PF_CHECK(Tests, !InvalidMaterialLoad && InvalidMaterialLoad.error().Code == SceneSerializationErrorCode::InvalidEntityData);
		PF_CHECK(Tests, Destination.FindEntity(RootId) == DestinationRootBeforeFailure);

		std::string InvalidCameraDocument = *Serialized;
		const size_t CameraFieldPosition = InvalidCameraDocument.find("\"verticalFovRadians\": ");
		const size_t CameraValuePosition = InvalidCameraDocument.find_first_of("0123456789", CameraFieldPosition);
		const size_t CameraValueEnd = InvalidCameraDocument.find_first_of(",\r\n", CameraValuePosition);
		PF_CHECK(Tests, CameraFieldPosition != std::string::npos && CameraValuePosition != std::string::npos &&
			CameraValueEnd != std::string::npos);
		if (CameraValuePosition != std::string::npos && CameraValueEnd != std::string::npos)
			InvalidCameraDocument.replace(CameraValuePosition, CameraValueEnd - CameraValuePosition, "0.0");
		const auto InvalidCameraResult = SceneSerializer::Deserialize(InvalidCameraDocument, Destination);
		PF_CHECK(Tests, !InvalidCameraResult.has_value());
		PF_CHECK(Tests, !InvalidCameraResult && InvalidCameraResult.error().Code == SceneSerializationErrorCode::InvalidEntityData);
		PF_CHECK(Tests, Destination.FindEntity(RootId) == DestinationRootBeforeFailure);

		std::string InvalidPrimaryCameraDocument = *Serialized;
		const size_t PrimaryValuePosition = InvalidPrimaryCameraDocument.find("\"primary\": true");
		PF_CHECK(Tests, PrimaryValuePosition != std::string::npos);
		if (PrimaryValuePosition != std::string::npos)
			InvalidPrimaryCameraDocument.replace(PrimaryValuePosition, std::string("\"primary\": true").size(), "\"primary\": \"yes\"");
		const auto InvalidPrimaryCameraResult = SceneSerializer::Deserialize(InvalidPrimaryCameraDocument, Destination);
		PF_CHECK(Tests, !InvalidPrimaryCameraResult &&
			InvalidPrimaryCameraResult.error().Code == SceneSerializationErrorCode::InvalidEntityData);
		PF_CHECK(Tests, Destination.FindEntity(RootId) == DestinationRootBeforeFailure);

		std::string UnsupportedFormat = *Serialized;
		const size_t FormatPosition = UnsupportedFormat.find("PulseForgeScene");
		PF_CHECK(Tests, FormatPosition != std::string::npos);
		if (FormatPosition != std::string::npos)
			UnsupportedFormat.replace(FormatPosition, std::string("PulseForgeScene").size(), "OtherSceneFormat");
		const auto UnsupportedFormatResult = SceneSerializer::Deserialize(UnsupportedFormat, Destination);
		PF_CHECK(Tests, !UnsupportedFormatResult.has_value());
		PF_CHECK(Tests, !UnsupportedFormatResult && UnsupportedFormatResult.error().Code == SceneSerializationErrorCode::UnsupportedFormat);
		PF_CHECK(Tests, Destination.FindEntity(RootId) == DestinationRootBeforeFailure);

		std::string InvalidTransform = *Serialized;
		const size_t TranslationPosition = InvalidTransform.find("\"translation\": [");
		const size_t FirstTranslation = InvalidTransform.find("4.0", TranslationPosition);
		PF_CHECK(Tests, TranslationPosition != std::string::npos && FirstTranslation != std::string::npos);
		if (FirstTranslation != std::string::npos)
			InvalidTransform.replace(FirstTranslation, 3, "\"bad\"");
		const auto InvalidTransformResult = SceneSerializer::Deserialize(InvalidTransform, Destination);
		PF_CHECK(Tests, !InvalidTransformResult.has_value());
		PF_CHECK(Tests, !InvalidTransformResult && InvalidTransformResult.error().Code == SceneSerializationErrorCode::InvalidEntityData);
		PF_CHECK(Tests, Destination.FindEntity(RootId) == DestinationRootBeforeFailure);

		std::string MissingParent = *Serialized;
		const size_t ChildPosition = MissingParent.find(ChildId.ToString());
		const size_t ParentPosition = MissingParent.find(RootId.ToString(), ChildPosition);
		PF_CHECK(Tests, ChildPosition != std::string::npos && ParentPosition != std::string::npos);
		if (ParentPosition != std::string::npos)
			MissingParent.replace(ParentPosition, RootId.ToString().size(), UUID{ 0x3000000000000000ull, 3 }.ToString());
		const auto MissingParentResult = SceneSerializer::Deserialize(MissingParent, Destination);
		PF_CHECK(Tests, !MissingParentResult.has_value());
		PF_CHECK(Tests, !MissingParentResult && MissingParentResult.error().Code == SceneSerializationErrorCode::MissingParent);
		PF_CHECK(Tests, Destination.FindEntity(RootId) == DestinationRootBeforeFailure);

		std::string HierarchyCycle = *Serialized;
		const size_t DetachedParentPosition = HierarchyCycle.find("\"parent\": null");
		PF_CHECK(Tests, DetachedParentPosition != std::string::npos);
		if (DetachedParentPosition != std::string::npos)
		{
			HierarchyCycle.replace(
				DetachedParentPosition,
				std::string("\"parent\": null").size(),
				"\"parent\": \"" + ChildId.ToString() + "\"");
		}
		const auto HierarchyCycleResult = SceneSerializer::Deserialize(HierarchyCycle, Destination);
		PF_CHECK(Tests, !HierarchyCycleResult.has_value());
		PF_CHECK(Tests, !HierarchyCycleResult && HierarchyCycleResult.error().Code == SceneSerializationErrorCode::InvalidEntityData);
		PF_CHECK(Tests, Destination.FindEntity(RootId) == DestinationRootBeforeFailure);

		std::string DuplicateUUID = *Serialized;
		const std::string RootRecordPrefix = "\"uuid\": \"";
		const size_t RootRecordPosition = DuplicateUUID.find(RootRecordPrefix + RootId.ToString() + "\"");
		PF_CHECK(Tests, RootRecordPosition != std::string::npos);
		if (RootRecordPosition != std::string::npos)
			DuplicateUUID.replace(RootRecordPosition + RootRecordPrefix.size(), RootId.ToString().size(), ChildId.ToString());
		const auto DuplicateUUIDResult = SceneSerializer::Deserialize(DuplicateUUID, Destination);
		PF_CHECK(Tests, !DuplicateUUIDResult.has_value());
		PF_CHECK(Tests, !DuplicateUUIDResult && DuplicateUUIDResult.error().Code == SceneSerializationErrorCode::InvalidEntityData);
		PF_CHECK(Tests, Destination.FindEntity(RootId) == DestinationRootBeforeFailure);

		const auto InvalidJsonResult = SceneSerializer::Deserialize("{invalid json", Destination);
		PF_CHECK(Tests, !InvalidJsonResult.has_value());
		PF_CHECK(Tests, Destination.FindEntity(RootId) == DestinationRootBeforeFailure);

		Scene EmptyScene;
		const auto EmptySerialized = SceneSerializer::Serialize(EmptyScene);
		PF_CHECK(Tests, EmptySerialized.has_value());
		if (EmptySerialized)
		{
			PF_CHECK(Tests, SceneSerializer::Deserialize(*EmptySerialized, Destination).has_value());
			PF_CHECK(Tests, Destination.GetEntityCount() == 0);
			PF_CHECK(Tests, !LoadedRoot->IsValid());
		}

		std::error_code TemporaryDirectoryError;
		const std::filesystem::path TemporaryDirectory = std::filesystem::temp_directory_path(TemporaryDirectoryError);
		PF_CHECK(Tests, !TemporaryDirectoryError);
		if (TemporaryDirectoryError)
			return;
		const auto FileIdentifier = UUID::Generate();
		PF_CHECK(Tests, FileIdentifier.has_value());
		if (!FileIdentifier)
			return;

		const std::filesystem::path ScenePath = TemporaryDirectory / ("PulseForgeScene-" + FileIdentifier->ToString() + ".json");
		struct SceneFileCleanup
		{
			std::filesystem::path Path;
			~SceneFileCleanup()
			{
				std::error_code Error;
				std::filesystem::remove(Path, Error);
			}
		} Cleanup{ ScenePath };

		const auto SaveResult = SceneSerializer::SaveToFile(Source, ScenePath);
		PF_CHECK(Tests, SaveResult.has_value());
		if (!SaveResult)
			return;

		Scene FileLoadedScene;
		const auto LoadResult = SceneSerializer::LoadFromFile(ScenePath, FileLoadedScene);
		PF_CHECK(Tests, LoadResult.has_value());
		PF_CHECK(Tests, FileLoadedScene.GetEntityCount() == 2);
		const auto FileLoadedChild = FileLoadedScene.FindEntity(ChildId);
		PF_CHECK(Tests, FileLoadedChild.has_value());
		if (FileLoadedChild)
		{
			const auto FileLoadedParent = FileLoadedChild->GetParent();
			PF_CHECK(Tests, FileLoadedParent.has_value() && FileLoadedParent->has_value());
		}

		const auto OverwriteResult = SceneSerializer::SaveToFile(EmptyScene, ScenePath);
		PF_CHECK(Tests, OverwriteResult.has_value());
		if (OverwriteResult)
		{
			PF_CHECK(Tests, SceneSerializer::LoadFromFile(ScenePath, FileLoadedScene).has_value());
			PF_CHECK(Tests, FileLoadedScene.GetEntityCount() == 0);
			PF_CHECK(Tests, FileLoadedChild && !FileLoadedChild->IsValid());
		}

		std::filesystem::path MissingScenePath = ScenePath;
		MissingScenePath += ".missing";
		const auto EmptyPathResult = SceneSerializer::SaveToFile(Source, {});
		PF_CHECK(Tests, !EmptyPathResult.has_value());
		const auto MissingFileResult = SceneSerializer::LoadFromFile(MissingScenePath, Source);
		PF_CHECK(Tests, !MissingFileResult.has_value());
		PF_CHECK(Tests, !MissingFileResult && MissingFileResult.error().Code == SceneSerializationErrorCode::FileOpenFailed);
		PF_CHECK(Tests, Source.GetEntityCount() == 2);
	}

	void TestSceneAssetsByUUID(TestRunner& Tests)
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

		const std::filesystem::path ProjectRoot = TemporaryDirectory / ("PulseForgeSceneAssets-" + ProjectIdentifier->ToString());
		struct ProjectCleanup
		{
			std::filesystem::path Path;
			~ProjectCleanup()
			{
				std::error_code Error;
				std::filesystem::remove_all(Path, Error);
			}
		} Cleanup{ ProjectRoot };

		const std::filesystem::path SceneDirectory = ProjectRoot / "Assets" / "Scenes";
		std::filesystem::create_directories(SceneDirectory, FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError)
			return;

		const auto CameraID = UUID::Parse("1b6cafe2-0f30-44ea-b841-10cf66f3e517");
		const auto MeshEntityID = UUID::Parse("2c76c6f7-e8f5-4936-9a24-61dbe7527e08");
		const auto MeshAssetID = UUID::Parse("3d56dd08-fd22-4a47-9ddf-09f392f1a619");
		PF_CHECK(Tests, CameraID && MeshEntityID && MeshAssetID);
		if (!CameraID || !MeshEntityID || !MeshAssetID)
			return;

		Scene Source;
		const auto Camera = Source.CreateEntityWithUUID(*CameraID, "Camera");
		const auto MeshEntity = Source.CreateEntityWithUUID(*MeshEntityID, "Mesh");
		PF_CHECK(Tests, Camera && MeshEntity);
		if (!Camera || !MeshEntity)
			return;
		PF_CHECK(Tests, Camera->SetCamera(CameraComponent{}).has_value());
		PF_CHECK(Tests, MeshEntity->SetMeshRenderer(MeshRendererComponent{ *MeshAssetID }).has_value());

		const std::filesystem::path SourcePath = SceneDirectory / "validation.scene";
		PF_CHECK(Tests, SceneSerializer::SaveToFile(Source, SourcePath).has_value());
		const auto CreatedMetadata = AssetMetadataSerializer::CreateForNewAsset(SourcePath);
		PF_CHECK(Tests, CreatedMetadata.has_value());
		if (!CreatedMetadata)
			return;
		const AssetID SceneID = CreatedMetadata->ID;

		AssetRegistry Registry;
		PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());
		const auto CreatedScene = SceneAssetService::Create(
			Registry,
			ProjectRoot,
			"Assets/Scenes/generated.scene",
			Source);
		PF_CHECK(Tests, CreatedScene.has_value());
		PF_CHECK(Tests, CreatedScene && !CreatedScene->ID.IsNil() &&
			CreatedScene->ID != SceneID &&
			CreatedScene->ProjectRelativePath == "Assets/Scenes/generated.scene");
		if (CreatedScene)
		{
			const auto CreatedMetadata = AssetMetadataSerializer::LoadFromFile(
				ProjectRoot / "Assets/Scenes/generated.scene.meta");
			PF_CHECK(Tests, CreatedMetadata && CreatedMetadata->ID == CreatedScene->ID);
			PF_CHECK(Tests, Registry.Find(CreatedScene->ID).has_value());

			Scene GeneratedScene;
			PF_CHECK(Tests, SceneAssetService::Load(CreatedScene->ID, ProjectRoot, Registry, GeneratedScene).has_value());
			PF_CHECK(Tests, GeneratedScene.GetEntityCount() == Source.GetEntityCount());
			PF_CHECK(Tests, GeneratedScene.FindEntity(*MeshEntityID).has_value());

			const auto DuplicateCreation = SceneAssetService::Create(
				Registry,
				ProjectRoot,
				"Assets/Scenes/generated.scene",
				Source);
			PF_CHECK(Tests, !DuplicateCreation &&
				DuplicateCreation.error().Code == SceneAssetErrorCode::AssetOperationFailed);
			PF_CHECK(Tests, Registry.Find(CreatedScene->ID).has_value());
		}
		const auto InvalidSceneCreation = SceneAssetService::Create(
			Registry,
			ProjectRoot,
			"Assets/Scenes/generated.json",
			Source);
		PF_CHECK(Tests, !InvalidSceneCreation &&
			InvalidSceneCreation.error().Code == SceneAssetErrorCode::UnsupportedAssetType);

		const auto ResolvedSource = AssetPathResolver::ResolveManagedSourcePath(
			ProjectRoot,
			std::filesystem::path("Assets/Scenes/validation.scene"));
		PF_CHECK(Tests, ResolvedSource && *ResolvedSource == std::filesystem::absolute(SourcePath).lexically_normal());
		PF_CHECK(Tests, !AssetPathResolver::ResolveManagedSourcePath(ProjectRoot, "Assets/../outside.scene"));
		PF_CHECK(Tests, !AssetPathResolver::ResolveManagedSourcePath(ProjectRoot, SourcePath));

		Scene Loaded;
		const auto PreviousEntity = Loaded.CreateEntity("Previous scene contents");
		const auto LoadedScene = SceneAssetService::Load(SceneID, ProjectRoot, Registry, Loaded);
		PF_CHECK(Tests, LoadedScene.has_value());
		PF_CHECK(Tests, PreviousEntity && !PreviousEntity->IsValid());
		PF_CHECK(Tests, Loaded.GetEntityCount() == 2);
		const auto LoadedMesh = Loaded.FindEntity(*MeshEntityID);
		PF_CHECK(Tests, LoadedMesh.has_value());
		if (!LoadedMesh)
			return;
		const auto LoadedMeshRenderer = LoadedMesh->GetMeshRenderer();
		PF_CHECK(Tests, LoadedMeshRenderer && LoadedMeshRenderer->has_value());
		PF_CHECK(Tests, LoadedMeshRenderer && LoadedMeshRenderer->has_value() &&
			LoadedMeshRenderer->value().MeshAsset == *MeshAssetID);

		const auto MoveResult = AssetOperations::Move(
			Registry,
			ProjectRoot,
			SceneID,
			"Assets/Scenes/renamed.scene");
		PF_CHECK(Tests, MoveResult && MoveResult->ID == SceneID);
		const auto MovedRecord = Registry.Find(SceneID);
		PF_CHECK(Tests, MovedRecord && MovedRecord->ProjectRelativePath == "Assets/Scenes/renamed.scene");
		PF_CHECK(Tests, SceneAssetService::Load(SceneID, ProjectRoot, Registry, Loaded).has_value());

		const auto UpdatedMesh = Loaded.FindEntity(*MeshEntityID);
		PF_CHECK(Tests, UpdatedMesh.has_value());
		if (!UpdatedMesh)
			return;
		auto UpdatedTag = UpdatedMesh->GetTag();
		PF_CHECK(Tests, UpdatedTag.has_value());
		if (!UpdatedTag)
			return;
		UpdatedTag->Name = "Saved through stable scene UUID";
		PF_CHECK(Tests, UpdatedMesh->SetTag(*UpdatedTag).has_value());
		PF_CHECK(Tests, SceneAssetService::Save(SceneID, ProjectRoot, Registry, Loaded).has_value());
		const auto PreservedMetadata = AssetMetadataSerializer::LoadFromFile(
			AssetMetadataSerializer::GetSidecarPath(ProjectRoot / "Assets" / "Scenes" / "renamed.scene"));
		PF_CHECK(Tests, PreservedMetadata && PreservedMetadata->ID == SceneID);
		Scene SavedAgain;
		PF_CHECK(Tests, SceneAssetService::Load(SceneID, ProjectRoot, Registry, SavedAgain).has_value());
		const auto SavedEntity = SavedAgain.FindEntity(*MeshEntityID);
		PF_CHECK(Tests, SavedEntity && SavedEntity->GetTag() && SavedEntity->GetTag()->Name == "Saved through stable scene UUID");

		const std::filesystem::path OtherAssetPath = ProjectRoot / "Assets" / "Scenes" / "notes.txt";
		{
			std::ofstream Output(OtherAssetPath, std::ios::binary | std::ios::trunc);
			Output << "not a scene";
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto OtherMetadata = AssetMetadataSerializer::CreateForNewAsset(OtherAssetPath);
		PF_CHECK(Tests, OtherMetadata.has_value());

		const std::filesystem::path InvalidScenePath = ProjectRoot / "Assets" / "Scenes" / "broken.scene";
		{
			std::ofstream Output(InvalidScenePath, std::ios::binary | std::ios::trunc);
			Output << "{broken";
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto InvalidSceneMetadata = AssetMetadataSerializer::CreateForNewAsset(InvalidScenePath);
		PF_CHECK(Tests, InvalidSceneMetadata.has_value());
		PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());
		if (OtherMetadata)
		{
			const auto WrongType = SceneAssetService::Load(OtherMetadata->ID, ProjectRoot, Registry, SavedAgain);
			PF_CHECK(Tests, !WrongType && WrongType.error().Code == SceneAssetErrorCode::UnsupportedAssetType);
		}
		if (InvalidSceneMetadata)
		{
			const auto ExistingSavedEntity = SavedAgain.FindEntity(*MeshEntityID);
			const auto BrokenLoad = SceneAssetService::Load(InvalidSceneMetadata->ID, ProjectRoot, Registry, SavedAgain);
			PF_CHECK(Tests, !BrokenLoad && BrokenLoad.error().Code == SceneAssetErrorCode::SerializationFailed);
			PF_CHECK(Tests, ExistingSavedEntity && ExistingSavedEntity->IsValid());
		}

		const auto MissingID = UUID::Generate();
		PF_CHECK(Tests, MissingID.has_value());
		if (MissingID)
		{
			const auto Missing = SceneAssetService::Load(*MissingID, ProjectRoot, Registry, SavedAgain);
			PF_CHECK(Tests, !Missing && Missing.error().Code == SceneAssetErrorCode::AssetNotFound);
		}
	}

	void TestAssetMetadataAndRegistry(TestRunner& Tests)
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
		if (FileError)
			return;

		const auto WriteFile = [](const std::filesystem::path& Path, std::string_view Data)
		{
			std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
			if (!Output)
				return false;
			Output.write(Data.data(), static_cast<std::streamsize>(Data.size()));
			Output.close();
			return static_cast<bool>(Output);
		};
		const auto ReadFile = [](const std::filesystem::path& Path) -> std::optional<std::string>
		{
			std::ifstream Input(Path, std::ios::binary);
			if (!Input)
				return std::nullopt;
			std::string Data{ std::istreambuf_iterator<char>(Input), std::istreambuf_iterator<char>() };
			if (Input.bad())
				return std::nullopt;
			return Data;
		};
		const auto HasIssue = [](const AssetRegistryError& Error, AssetRegistryIssueCode Code)
		{
			return std::find_if(Error.Issues.begin(), Error.Issues.end(), [Code](const AssetRegistryIssue& Issue)
			{
				return Issue.Code == Code;
			}) != Error.Issues.end();
		};

		const std::filesystem::path FirstSource = ShaderDirectory / "first.hlsl";
		const std::filesystem::path SecondSource = ShaderDirectory / "second.hlsl";
		const std::filesystem::path DuplicateSource = ModelDirectory / "duplicate.hlsl";
		PF_CHECK(Tests, WriteFile(FirstSource, "first shader source"));
		PF_CHECK(Tests, WriteFile(SecondSource, "second shader source"));
		std::filesystem::copy_file(FirstSource, DuplicateSource, std::filesystem::copy_options::none, FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError)
			return;

		const auto FirstMetadata = AssetMetadataSerializer::CreateForNewAsset(FirstSource);
		const auto SecondMetadata = AssetMetadataSerializer::CreateForNewAsset(SecondSource);
		const auto DuplicateMetadata = AssetMetadataSerializer::CreateForNewAsset(DuplicateSource);
		PF_CHECK(Tests, FirstMetadata.has_value());
		PF_CHECK(Tests, SecondMetadata.has_value());
		PF_CHECK(Tests, DuplicateMetadata.has_value());
		if (!FirstMetadata || !SecondMetadata || !DuplicateMetadata)
			return;

		PF_CHECK(Tests, FirstMetadata->Version == AssetMetadataSerializer::CurrentVersion);
		PF_CHECK(Tests, FirstMetadata->ID != SecondMetadata->ID);
		PF_CHECK(Tests, FirstMetadata->ID != DuplicateMetadata->ID);
		PF_CHECK(Tests, AssetMetadataSerializer::GetSidecarPath(FirstSource).filename() == "first.hlsl.meta");
		const auto LoadedFirstMetadata = AssetMetadataSerializer::LoadFromFile(
			AssetMetadataSerializer::GetSidecarPath(FirstSource));
		PF_CHECK(Tests, LoadedFirstMetadata.has_value());
		PF_CHECK(Tests, LoadedFirstMetadata && LoadedFirstMetadata->ID == FirstMetadata->ID);
		const auto RecreateExistingMetadata = AssetMetadataSerializer::CreateForNewAsset(FirstSource);
		PF_CHECK(Tests, !RecreateExistingMetadata.has_value());
		PF_CHECK(Tests, !RecreateExistingMetadata &&
			RecreateExistingMetadata.error().Code == AssetMetadataErrorCode::MetadataAlreadyExists);
		const auto ExistingIdentity = AssetMetadataSerializer::LoadFromFile(
			AssetMetadataSerializer::GetSidecarPath(FirstSource));
		PF_CHECK(Tests, ExistingIdentity && ExistingIdentity->ID == FirstMetadata->ID);

		AssetRegistry Registry;
		const auto InitialBuild = Registry.Rebuild(ProjectRoot);
		PF_CHECK(Tests, InitialBuild.has_value());
		PF_CHECK(Tests, Registry.GetAssetCount() == 3);
		const std::vector<AssetRecord> RegisteredAssets = Registry.GetAssets();
		PF_CHECK(Tests, RegisteredAssets.size() == 3);
		if (RegisteredAssets.size() != 3)
			return;
		PF_CHECK(Tests, RegisteredAssets[0].ProjectRelativePath == std::filesystem::path("Assets/Models/duplicate.hlsl"));
		PF_CHECK(Tests, RegisteredAssets[1].ProjectRelativePath == std::filesystem::path("Assets/Shaders/first.hlsl"));
		PF_CHECK(Tests, RegisteredAssets[2].ProjectRelativePath == std::filesystem::path("Assets/Shaders/second.hlsl"));
		const auto FirstRecord = Registry.Find(FirstMetadata->ID);
		PF_CHECK(Tests, FirstRecord.has_value());
		if (!InitialBuild || !FirstRecord)
			return;
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
		if (FileError)
			return;
		std::filesystem::rename(FirstSidecar, RenamedSidecar, FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError)
			return;
		const auto StaleFirstRecord = Registry.Find(FirstMetadata->ID);
		PF_CHECK(Tests, StaleFirstRecord &&
			StaleFirstRecord->ProjectRelativePath == std::filesystem::path("Assets/Shaders/first.hlsl"));
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
		if (!OriginalSecondSidecar || !MovedRecord)
			return;
		PF_CHECK(Tests, OriginalSecondSidecar->find("\"importSettings\": {}") != std::string::npos);

		std::filesystem::remove(SecondSidecar, FileError);
		PF_CHECK(Tests, !FileError);
		const auto MissingMetadataBuild = Registry.Rebuild(ProjectRoot);
		PF_CHECK(Tests, !MissingMetadataBuild.has_value());
		PF_CHECK(Tests, !MissingMetadataBuild.has_value() &&
			HasIssue(MissingMetadataBuild.error(), AssetRegistryIssueCode::MissingMetadata));
		if (!MissingMetadataBuild)
		{
			const auto MissingIssue = std::find_if(
				MissingMetadataBuild.error().Issues.begin(),
				MissingMetadataBuild.error().Issues.end(),
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
		PF_CHECK(Tests, !RecreateCorruptMetadata &&
			RecreateCorruptMetadata.error().Code == AssetMetadataErrorCode::MetadataAlreadyExists);
		PF_CHECK(Tests, CorruptSidecarBeforeCreate && ReadFile(SecondSidecar) == CorruptSidecarBeforeCreate);
		const auto CorruptMetadataBuild = Registry.Rebuild(ProjectRoot);
		PF_CHECK(Tests, !CorruptMetadataBuild.has_value());
		PF_CHECK(Tests, !CorruptMetadataBuild.has_value() &&
			HasIssue(CorruptMetadataBuild.error(), AssetRegistryIssueCode::InvalidMetadata));
		if (!CorruptMetadataBuild)
		{
			const auto InvalidIssue = std::find_if(
				CorruptMetadataBuild.error().Issues.begin(),
				CorruptMetadataBuild.error().Issues.end(),
				[](const AssetRegistryIssue& Issue) { return Issue.Code == AssetRegistryIssueCode::InvalidMetadata; });
			PF_CHECK(Tests, InvalidIssue != CorruptMetadataBuild.error().Issues.end());
			if (InvalidIssue != CorruptMetadataBuild.error().Issues.end())
				PF_CHECK(Tests, InvalidIssue->ProjectRelativePath == std::filesystem::path("Assets/Shaders/second.hlsl"));
		}
		PF_CHECK(Tests, Registry.Find(SecondMetadata->ID).has_value());
		PF_CHECK(Tests, WriteFile(SecondSidecar, *OriginalSecondSidecar));

		const std::string UnsupportedVersion = "{\"format\":\"PulseForgeAssetMeta\",\"version\":2,\"uuid\":\"" +
			SecondMetadata->ID.ToString() + "\",\"importSettings\":{}}";
		PF_CHECK(Tests, WriteFile(SecondSidecar, UnsupportedVersion));
		const auto UnsupportedMetadataVersion = AssetMetadataSerializer::LoadFromFile(SecondSidecar);
		PF_CHECK(Tests, !UnsupportedMetadataVersion.has_value());
		PF_CHECK(Tests, !UnsupportedMetadataVersion &&
			UnsupportedMetadataVersion.error().Code == AssetMetadataErrorCode::UnsupportedVersion);
		std::string InvalidImportSettings = *OriginalSecondSidecar;
		const size_t ImportSettingsObject = InvalidImportSettings.find("\"importSettings\": {}");
		PF_CHECK(Tests, ImportSettingsObject != std::string::npos);
		if (ImportSettingsObject != std::string::npos)
			InvalidImportSettings.replace(ImportSettingsObject, std::string("\"importSettings\": {}").size(), "\"importSettings\": []");
		PF_CHECK(Tests, WriteFile(SecondSidecar, InvalidImportSettings));
		const auto InvalidImportSettingsLoad = AssetMetadataSerializer::LoadFromFile(SecondSidecar);
		PF_CHECK(Tests, !InvalidImportSettingsLoad.has_value());
		PF_CHECK(Tests, !InvalidImportSettingsLoad &&
			InvalidImportSettingsLoad.error().Code == AssetMetadataErrorCode::InvalidDocument);
		PF_CHECK(Tests, WriteFile(SecondSidecar, *OriginalSecondSidecar));

		std::filesystem::copy_file(
			RenamedSidecar,
			SecondSidecar,
			std::filesystem::copy_options::overwrite_existing,
			FileError);
		PF_CHECK(Tests, !FileError);
		const auto DuplicateIdentityBuild = Registry.Rebuild(ProjectRoot);
		PF_CHECK(Tests, !DuplicateIdentityBuild.has_value());
		PF_CHECK(Tests, !DuplicateIdentityBuild.has_value() &&
			HasIssue(DuplicateIdentityBuild.error(), AssetRegistryIssueCode::DuplicateAssetID));
		if (!DuplicateIdentityBuild)
		{
			const auto DuplicateIssue = std::find_if(
				DuplicateIdentityBuild.error().Issues.begin(),
				DuplicateIdentityBuild.error().Issues.end(),
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
		PF_CHECK(Tests, PreservedFirstRecord &&
			PreservedFirstRecord->ProjectRelativePath == std::filesystem::path("Assets/Models/renamed.hlsl"));
		const auto DuplicateCreation = AssetMetadataSerializer::CreateForNewAsset(SecondSource);
		PF_CHECK(Tests, !DuplicateCreation.has_value());
		PF_CHECK(Tests, !DuplicateCreation && DuplicateCreation.error().Code == AssetMetadataErrorCode::MetadataAlreadyExists);
		PF_CHECK(Tests, WriteFile(SecondSidecar, *OriginalSecondSidecar));
		PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());

		std::filesystem::remove(DuplicateSource, FileError);
		PF_CHECK(Tests, !FileError);
		const auto OrphanedSidecarBuild = Registry.Rebuild(ProjectRoot);
		PF_CHECK(Tests, !OrphanedSidecarBuild.has_value());
		PF_CHECK(Tests, !OrphanedSidecarBuild.has_value() &&
			HasIssue(OrphanedSidecarBuild.error(), AssetRegistryIssueCode::MissingSourceAsset));
		PF_CHECK(Tests, Registry.Find(DuplicateMetadata->ID).has_value());

		const std::filesystem::path ReservedSuffixSource = ShaderDirectory / "reserved-suffix";
		PF_CHECK(Tests, WriteFile(ReservedSuffixSource, "regular source asset"));
		const auto ReservedSuffixMetadata = AssetMetadataSerializer::CreateForNewAsset(ReservedSuffixSource);
		PF_CHECK(Tests, ReservedSuffixMetadata.has_value());
		const auto NestedSidecarID = UUID::Generate();
		PF_CHECK(Tests, NestedSidecarID.has_value());
		if (!ReservedSuffixMetadata || !NestedSidecarID)
			return;

		const std::filesystem::path NestedSidecarPath =
			AssetMetadataSerializer::GetSidecarPath(AssetMetadataSerializer::GetSidecarPath(ReservedSuffixSource));
		const std::string NestedSidecar = "{\"format\":\"PulseForgeAssetMeta\",\"version\":1,\"uuid\":\"" +
			NestedSidecarID->ToString() + "\",\"importSettings\":{}}";
		PF_CHECK(Tests, WriteFile(NestedSidecarPath, NestedSidecar));
		const auto ReservedSuffixBuild = Registry.Rebuild(ProjectRoot);
		PF_CHECK(Tests, !ReservedSuffixBuild.has_value());
		PF_CHECK(Tests, !ReservedSuffixBuild.has_value() &&
			HasIssue(ReservedSuffixBuild.error(), AssetRegistryIssueCode::UnsupportedEntry));
	}

	void TestAssetOperations(TestRunner& Tests)
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
		if (FileError)
			return;

		const auto WriteFile = [](const std::filesystem::path& Path, std::string_view Data)
		{
			std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
			if (!Output)
				return false;
			Output.write(Data.data(), static_cast<std::streamsize>(Data.size()));
			Output.close();
			return static_cast<bool>(Output);
		};
		const auto ReadFile = [](const std::filesystem::path& Path) -> std::optional<std::string>
		{
			std::ifstream Input(Path, std::ios::binary);
			if (!Input)
				return std::nullopt;
			std::string Data{ std::istreambuf_iterator<char>(Input), std::istreambuf_iterator<char>() };
			if (Input.bad())
				return std::nullopt;
			return Data;
		};

		const std::filesystem::path Source = AssetDirectory / "ship.gltf";
		PF_CHECK(Tests, WriteFile(Source, "managed model source"));
		const auto SourceMetadata = AssetMetadataSerializer::CreateForNewAsset(Source);
		PF_CHECK(Tests, SourceMetadata.has_value());
		if (!SourceMetadata)
			return;
		const std::filesystem::path SourceSidecar = AssetMetadataSerializer::GetSidecarPath(Source);
		const std::string MetadataWithImportSettings =
			"{\"format\":\"PulseForgeAssetMeta\",\"version\":1,\"uuid\":\"" +
			SourceMetadata->ID.ToString() + "\",\"importSettings\":{\"scale\":2.5}}";
		PF_CHECK(Tests, WriteFile(SourceSidecar, MetadataWithImportSettings));

		AssetRegistry InitialRegistry;
		PF_CHECK(Tests, InitialRegistry.Rebuild(ProjectRoot).has_value());
		PF_CHECK(Tests, InitialRegistry.GetAssetCount() == 1);

		const auto UnknownAsset = UUID::Generate();
		PF_CHECK(Tests, UnknownAsset.has_value());
		if (!UnknownAsset)
			return;
		const auto UnknownMove = AssetOperations::Move(
			InitialRegistry,
			ProjectRoot,
			*UnknownAsset,
			"Assets/Models/not-found-move.gltf");
		PF_CHECK(Tests, !UnknownMove && UnknownMove.error().Code == AssetOperationErrorCode::AssetNotFound);
		PF_CHECK(Tests, !UnknownMove && UnknownMove.error().Message.find(UnknownAsset->ToString()) != std::string::npos);
		const auto UnknownDuplicate = AssetOperations::Duplicate(
			InitialRegistry,
			ProjectRoot,
			*UnknownAsset,
			"Assets/Models/not-found-duplicate.gltf");
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
			const AssetOperations::RegistryRefresh FailPostMoveRefresh = [&](const auto& Root)
				-> std::expected<void, AssetRegistryError>
			{
				if (++RefreshCount == 2)
				{
					AssetRegistryError Failure;
					Failure.Issues.push_back({ AssetRegistryIssueCode::AssetScanFailed, "Assets", {},
						"Injected final registry refresh failure" });
					return std::unexpected(std::move(Failure));
				}
				return InitialRegistry.Rebuild(Root);
			};
			const auto RolledBackMove = AssetOperations::MoveWithRegistryRefreshForTesting(
				InitialRegistry, ProjectRoot, SourceMetadata->ID, RollbackPath, FailPostMoveRefresh);
			const auto RolledBackRecord = InitialRegistry.Find(SourceMetadata->ID);
			PF_CHECK(Tests, !RolledBackMove &&
				RolledBackMove.error().Code == AssetOperationErrorCode::RegistryRefreshFailed);
			PF_CHECK(Tests, RefreshCount == 3 && std::filesystem::exists(MovedSource) &&
				!std::filesystem::exists(ProjectRoot / RollbackPath));
			PF_CHECK(Tests, RolledBackRecord && RolledBackRecord->ProjectRelativePath == MovedPath);
			PF_CHECK(Tests, ReadFile(MovedSidecarPath) == std::optional<std::string>(MetadataWithImportSettings));

			const std::filesystem::path RecoveryPath = "Assets/Models/registry-recovery.gltf";
			bool ConflictCreated = false;
			RefreshCount = 0;
			const AssetOperations::RegistryRefresh FailRollbackRefresh = [&](const auto& Root)
				-> std::expected<void, AssetRegistryError>
			{
				if (++RefreshCount == 2)
				{
					ConflictCreated = WriteFile(MovedSource, "concurrent source path");
					AssetRegistryError Failure;
					Failure.Issues.push_back({ AssetRegistryIssueCode::AssetScanFailed, "Assets", {},
						"Injected final registry refresh failure" });
					return std::unexpected(std::move(Failure));
				}
				return InitialRegistry.Rebuild(Root);
			};
			const auto RecoveryMove = AssetOperations::MoveWithRegistryRefreshForTesting(
				InitialRegistry, ProjectRoot, SourceMetadata->ID, RecoveryPath, FailRollbackRefresh);
			const auto RecoveryAsset = ProjectRoot / RecoveryPath;
			PF_CHECK(Tests, !RecoveryMove && RecoveryMove.error().Code == AssetOperationErrorCode::RecoveryRequired);
			PF_CHECK(Tests, ConflictCreated && ReadFile(MovedSource) == std::optional<std::string>("concurrent source path"));
			PF_CHECK(Tests, std::filesystem::exists(RecoveryAsset) &&
				std::filesystem::exists(AssetMetadataSerializer::GetSidecarPath(RecoveryAsset)));
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
		const auto DuplicateMetadata = AssetMetadataSerializer::LoadFromFile(
			AssetMetadataSerializer::GetSidecarPath(DuplicateSource));
		PF_CHECK(Tests, DuplicateMetadata && DuplicateMetadata->ID == DuplicateResult->ID);

		const auto ExistingDestination = AssetOperations::Duplicate(InitialRegistry, ProjectRoot, SourceMetadata->ID, DuplicatePath);
		PF_CHECK(Tests, !ExistingDestination.has_value());
		PF_CHECK(Tests, !ExistingDestination && ExistingDestination.error().Code == AssetOperationErrorCode::DestinationExists);
		const auto ExistingMoveDestination = AssetOperations::Move(InitialRegistry, ProjectRoot, SourceMetadata->ID, DuplicatePath);
		PF_CHECK(Tests, !ExistingMoveDestination.has_value());
		PF_CHECK(Tests, !ExistingMoveDestination &&
			ExistingMoveDestination.error().Code == AssetOperationErrorCode::DestinationExists);
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
		PF_CHECK(Tests, ReadFile(AssetMetadataSerializer::GetSidecarPath(CaseChangedSource)) ==
			std::optional<std::string>(MetadataWithImportSettings));
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
		const bool TransactionDirectoryEmpty = std::filesystem::is_empty(
			ProjectRoot / ".pulseforge" / "cache" / "asset-operations", FileError);
		PF_CHECK(Tests, !FileError && TransactionDirectoryEmpty);

		const std::filesystem::path AliasedSource = AssetDirectory / "aliased.gltf";
		PF_CHECK(Tests, WriteFile(AliasedSource, "duplicate identity source"));
		PF_CHECK(Tests, WriteFile(
			AssetMetadataSerializer::GetSidecarPath(AliasedSource),
			"{\"format\":\"PulseForgeAssetMeta\",\"version\":1,\"uuid\":\"" +
				SourceMetadata->ID.ToString() + "\",\"importSettings\":{}}"));
		const std::filesystem::path CollisionCopyPath = "Assets/Models/collision-copy.gltf";
		const auto DuplicateInInvalidProject = AssetOperations::Duplicate(
			InitialRegistry, ProjectRoot, SourceMetadata->ID, CollisionCopyPath);
		PF_CHECK(Tests, !DuplicateInInvalidProject.has_value());
		PF_CHECK(Tests, !DuplicateInInvalidProject &&
			DuplicateInInvalidProject.error().Code == AssetOperationErrorCode::InvalidProjectAssets);
		PF_CHECK(Tests, !std::filesystem::exists(ProjectRoot / CollisionCopyPath));
		std::filesystem::remove(AliasedSource, FileError);
		PF_CHECK(Tests, !FileError);
		std::filesystem::remove(AssetMetadataSerializer::GetSidecarPath(AliasedSource), FileError);
		PF_CHECK(Tests, !FileError);

		std::filesystem::remove(AssetMetadataSerializer::GetSidecarPath(MovedSource), FileError);
		PF_CHECK(Tests, !FileError);
		const auto MissingMetadataOperation = AssetOperations::Duplicate(
			InitialRegistry, ProjectRoot, SourceMetadata->ID, "Assets/Models/untracked.gltf");
		PF_CHECK(Tests, !MissingMetadataOperation.has_value());
		PF_CHECK(Tests, !MissingMetadataOperation &&
			MissingMetadataOperation.error().Code == AssetOperationErrorCode::InvalidProjectAssets);
		PF_CHECK(Tests, !std::filesystem::exists(ProjectRoot / "Assets/Models/untracked.gltf"));
	}

	void TestAssetFolderOperations(TestRunner& Tests)
	{
		using namespace PulseForge;
		std::error_code Error;
		const auto ProjectIdentifier = UUID::Generate();
		PF_CHECK(Tests, ProjectIdentifier.has_value());
		if (!ProjectIdentifier)
			return;

		const std::filesystem::path ProjectRoot = std::filesystem::temp_directory_path() /
			("PulseForgeAssetFolderOperations-" + ProjectIdentifier->ToString());
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
		if (Error)
			return;

		const auto WriteManagedAsset = [](const std::filesystem::path& Path, std::string_view Contents)
			-> std::optional<AssetID>
		{
			std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
			if (!Output)
				return std::nullopt;
			Output.write(Contents.data(), static_cast<std::streamsize>(Contents.size()));
			Output.close();
			if (!Output)
				return std::nullopt;
			const auto Metadata = AssetMetadataSerializer::CreateForNewAsset(Path);
			return Metadata ? std::optional<AssetID>(Metadata->ID) : std::nullopt;
		};

		const auto RootAsset = WriteManagedAsset(ProjectRoot / "Assets" / "Source" / "root.material", "root");
		const auto NestedAsset = WriteManagedAsset(
			ProjectRoot / "Assets" / "Source" / "Nested" / "nested.png", "nested");
		PF_CHECK(Tests, RootAsset.has_value() && NestedAsset.has_value());
		if (!RootAsset || !NestedAsset)
			return;

		AssetRegistry Registry;
		PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());
		PF_CHECK(Tests, Registry.GetAssetCount() == 2);

		const auto SelfMove = AssetOperations::MoveFolder(
			Registry, ProjectRoot, "Assets/Source", "Assets/Source");
		PF_CHECK(Tests, !SelfMove && SelfMove.error().Code == AssetOperationErrorCode::InvalidPath);
		const auto DescendantMove = AssetOperations::MoveFolder(
			Registry, ProjectRoot, "Assets/Source", "Assets/Source/Nested");
		PF_CHECK(Tests, !DescendantMove && DescendantMove.error().Code == AssetOperationErrorCode::InvalidPath);
		const auto TraversalMove = AssetOperations::MoveFolder(
			Registry, ProjectRoot, "Assets/Source", "Assets/../Destination");
		PF_CHECK(Tests, !TraversalMove && TraversalMove.error().Code == AssetOperationErrorCode::InvalidPath);
		PF_CHECK(Tests, std::filesystem::exists(ProjectRoot / "Assets" / "Source" / "root.material"));

		const auto Moved = AssetOperations::MoveFolder(
			Registry, ProjectRoot, "Assets/Source", "Assets/Destination");
		PF_CHECK(Tests, Moved.has_value());
		PF_CHECK(Tests, Moved && *Moved == std::filesystem::path("Assets/Destination/Source"));
		PF_CHECK(Tests, !std::filesystem::exists(ProjectRoot / "Assets" / "Source"));
		PF_CHECK(Tests, std::filesystem::exists(ProjectRoot / "Assets" / "Destination" / "Source" / "Nested" / "nested.png"));
		const auto MovedRootRecord = Registry.Find(*RootAsset);
		const auto MovedNestedRecord = Registry.Find(*NestedAsset);
		PF_CHECK(Tests, MovedRootRecord && MovedRootRecord->ProjectRelativePath ==
			std::filesystem::path("Assets/Destination/Source/root.material"));
		PF_CHECK(Tests, MovedNestedRecord && MovedNestedRecord->ProjectRelativePath ==
			std::filesystem::path("Assets/Destination/Source/Nested/nested.png"));
		PF_CHECK(Tests, MovedRootRecord && MovedRootRecord->ID == *RootAsset);
		PF_CHECK(Tests, MovedNestedRecord && MovedNestedRecord->ID == *NestedAsset);

		std::filesystem::create_directories(ProjectRoot / "Assets" / "Other" / "Source", Error);
		PF_CHECK(Tests, !Error);
		const auto Collision = AssetOperations::MoveFolder(
			Registry, ProjectRoot, "Assets/Other/Source", "Assets/Destination");
		PF_CHECK(Tests, !Collision && Collision.error().Code == AssetOperationErrorCode::DestinationExists);
		PF_CHECK(Tests, std::filesystem::exists(ProjectRoot / "Assets" / "Destination" / "Source" / "root.material"));

		const auto EmptySource = AssetOperations::MoveFolder(
			Registry, ProjectRoot, "Assets/Destination/Source", "Assets");
		PF_CHECK(Tests, EmptySource.has_value());
		PF_CHECK(Tests, EmptySource && *EmptySource == std::filesystem::path("Assets/Source"));
		const auto ReturnedToRoot = Registry.Find(*RootAsset);
		PF_CHECK(Tests, ReturnedToRoot && ReturnedToRoot->ProjectRelativePath ==
			std::filesystem::path("Assets/Source/root.material"));

		const std::filesystem::path LinkPath = ProjectRoot / "Assets" / "Linked";
		Error.clear();
		std::filesystem::create_directory_symlink(ProjectRoot / "Assets" / "Destination", LinkPath, Error);
		if (!Error)
		{
			const auto ReparseTarget = AssetOperations::MoveFolder(
				Registry, ProjectRoot, "Assets/Source", "Assets/Linked");
			PF_CHECK(Tests, !ReparseTarget && ReparseTarget.error().Code == AssetOperationErrorCode::InvalidPath);
			std::filesystem::remove(LinkPath, Error);
			PF_CHECK(Tests, !Error);
		}
	}
}

void RunAssetsTests(TestRunner& Tests)
{
	TestSceneSerializationRoundTrip(Tests);
	TestSceneAssetsByUUID(Tests);
	TestAssetMetadataAndRegistry(Tests);
	TestAssetOperations(Tests);
	TestAssetFolderOperations(Tests);
}
