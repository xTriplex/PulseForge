#include "Core/PulseForgePCH.h"
#include "Scene/SceneSerializer.h"
#include "Scene/Components/AudioListenerComponent.h"
#include "Scene/Components/AudioSourceComponent.h"
#include "Scene/Components/DirectionalLightComponent.h"
#include "Scene/Components/EnvironmentLightComponent.h"
#include "Scene/Components/PointLightComponent.h"
#include "Scene/Components/SpotLightComponent.h"
#include "Scene/Components/MeshRendererComponent.h"
#include "Scene/Components/ScriptComponent.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cmath>
#include <exception>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <system_error>
#include <unordered_set>
#include <vector>

namespace PulseForge
{
	namespace
	{
		using Json = nlohmann::ordered_json;
		constexpr int64_t SceneFormatVersion = 7;
		constexpr int64_t MinimumSupportedSceneFormatVersion = 1;
		constexpr std::string_view SceneFormatName = "PulseForgeScene";

		SceneSerializationError MakeError(SceneSerializationErrorCode Code, std::string Message)
		{
			return { Code, std::move(Message) };
		}

		bool ReadFiniteFloats(const Json& Value, std::span<float> Output)
		{
			if (!Value.is_array() || Value.size() != Output.size())
				return false;

			for (size_t Index = 0; Index < Output.size(); ++Index)
			{
				if (!Value[Index].is_number())
					return false;
				Output[Index] = Value[Index].get<float>();
				if (!std::isfinite(Output[Index]))
					return false;
			}
			return true;
		}

		SceneSerializationError SceneOperationError(const SceneError& Error)
		{
			if (Error.Code == SceneErrorCode::DuplicateUUID || Error.Code == SceneErrorCode::NilUUID ||
				Error.Code == SceneErrorCode::InvalidTransform || Error.Code == SceneErrorCode::ParentCycle ||
				Error.Code == SceneErrorCode::InvalidCamera || Error.Code == SceneErrorCode::InvalidDirectionalLight ||
				Error.Code == SceneErrorCode::InvalidEnvironmentLight ||
				Error.Code == SceneErrorCode::InvalidPointLight || Error.Code == SceneErrorCode::InvalidSpotLight ||
				Error.Code == SceneErrorCode::InvalidAssetReference ||
				Error.Code == SceneErrorCode::InvalidPhysicsComponent || Error.Code == SceneErrorCode::InvalidAudioComponent ||
				Error.Code == SceneErrorCode::InvalidScriptComponent)
			{
				return MakeError(SceneSerializationErrorCode::InvalidEntityData, Error.Message);
			}
			return MakeError(SceneSerializationErrorCode::SceneOperationFailed, Error.Message);
		}

		std::string PathForMessage(const std::filesystem::path& Path)
		{
			const std::u8string UTF8Path = Path.u8string();
			return { reinterpret_cast<const char*>(UTF8Path.data()), UTF8Path.size() };
		}

		struct TemporaryFileCleanup
		{
			std::filesystem::path Path;

			~TemporaryFileCleanup()
			{
				std::error_code Error;
				std::filesystem::remove(Path, Error);
			}
		};
	}

	std::expected<std::string, SceneSerializationError> SceneSerializer::Serialize(const Scene& Source)
	{
		try
		{
			Json Document = Json::object();
			Document["format"] = SceneFormatName;
			Document["version"] = SceneFormatVersion;
			Document["entities"] = Json::array();

			for (const Entity& Current : Source.GetEntities())
			{
				const auto Tag = Current.GetTag();
				const auto Transform = Current.GetTransform();
				const auto Camera = Current.GetCamera();
				const auto DirectionalLight = Current.GetDirectionalLight();
				const auto EnvironmentLight = Current.GetEnvironmentLight();
				const auto PointLight = Current.GetPointLight();
				const auto SpotLight = Current.GetSpotLight();
				const auto MeshRenderer = Current.GetMeshRenderer();
				const auto Rigidbody = Current.GetRigidbody();
				const auto BoxCollider = Current.GetBoxCollider();
				const auto AudioSource = Current.GetAudioSource();
				const auto AudioListener = Current.GetAudioListener();
				const auto Script = Current.GetScript();
				const auto Parent = Current.GetParent();
				if (!Tag || !Transform || !Camera || !DirectionalLight || !EnvironmentLight || !PointLight || !SpotLight || !MeshRenderer || !Rigidbody || !BoxCollider ||
					!AudioSource || !AudioListener || !Script || !Parent)
					return std::unexpected(MakeError(
						SceneSerializationErrorCode::SceneOperationFailed,
						"Could not read all required components while serializing an entity"));

				Json Record = Json::object();
				Record["uuid"] = Current.GetUUID().ToString();
				Record["tag"] = Json::object({ { "name", Tag->Name } });
				Record["transform"] = Json::object({
					{ "translation", { Transform->Translation.x, Transform->Translation.y, Transform->Translation.z } },
					{ "rotation", { Transform->Rotation.w, Transform->Rotation.x, Transform->Rotation.y, Transform->Rotation.z } },
					{ "scale", { Transform->Scale.x, Transform->Scale.y, Transform->Scale.z } }
				});
				if (Camera->has_value())
				{
					const CameraComponent& CameraData = Camera->value();
					Record["camera"] = Json::object({
						{ "verticalFovRadians", CameraData.VerticalFieldOfViewRadians },
						{ "nearClipPlane", CameraData.NearClipPlane },
						{ "farClipPlane", CameraData.FarClipPlane },
						{ "primary", CameraData.IsPrimary }
					});
				}
				if (DirectionalLight->has_value())
				{
					const DirectionalLightComponent& Light = DirectionalLight->value();
					Record["directionalLight"] = Json::object({
						{ "color", { Light.Color.r, Light.Color.g, Light.Color.b } },
						{ "intensity", Light.Intensity },
						{ "castShadows", Light.CastShadows },
						{ "shadowDistance", Light.ShadowDistance },
						{ "shadowBias", Light.ShadowBias },
						{ "shadowNormalBias", Light.ShadowNormalBias },
						{ "shadowSoftness", Light.ShadowSoftness }
					});
				}
				if (EnvironmentLight->has_value())
				{
					const EnvironmentLightComponent& Environment = EnvironmentLight->value();
					Record["environmentLight"] = Json::object({
						{ "hdrImage", Environment.HdrImage.ToString() },
						{ "intensity", Environment.Intensity }
					});
				}
				if (PointLight->has_value())
				{
					const auto& Light = PointLight->value();
					Record["pointLight"] = Json::object({
						{ "color", { Light.Color.r, Light.Color.g, Light.Color.b } },
						{ "intensity", Light.Intensity }, { "range", Light.Range }
					});
				}
				if (SpotLight->has_value())
				{
					const auto& Light = SpotLight->value();
					Record["spotLight"] = Json::object({
						{ "color", { Light.Color.r, Light.Color.g, Light.Color.b } },
						{ "intensity", Light.Intensity }, { "range", Light.Range },
						{ "innerConeAngleDegrees", Light.InnerConeAngleDegrees },
						{ "outerConeAngleDegrees", Light.OuterConeAngleDegrees }
					});
				}
				if (MeshRenderer->has_value())
				{
					Json MeshRendererRecord = Json::object({
						{ "meshAsset", MeshRenderer->value().MeshAsset.ToString() }
					});
					if (MeshRenderer->value().MaterialAsset)
						MeshRendererRecord["materialAsset"] = MeshRenderer->value().MaterialAsset->ToString();
					Record["meshRenderer"] = std::move(MeshRendererRecord);
				}
				if (Rigidbody->has_value())
				{
					const RigidbodyComponent& RigidbodyData = Rigidbody->value();
					Record["rigidbody"] = Json::object({
						{ "motionType", RigidbodyData.MotionType == RigidbodyMotionType::Static ? "static" : "dynamic" },
						{ "mass", RigidbodyData.Mass },
						{ "friction", RigidbodyData.Friction },
						{ "restitution", RigidbodyData.Restitution },
						{ "allowSleeping", RigidbodyData.AllowSleeping }
					});
				}
				if (BoxCollider->has_value())
				{
					const glm::vec3 HalfExtents = BoxCollider->value().HalfExtents;
					Record["boxCollider"] = Json::object({
						{ "halfExtents", { HalfExtents.x, HalfExtents.y, HalfExtents.z } }
					});
				}
				if (AudioSource->has_value())
				{
					const AudioSourceComponent& AudioSourceData = AudioSource->value();
					Record["audioSource"] = Json::object({
						{ "asset", AudioSourceData.AudioAsset.ToString() },
						{ "volume", AudioSourceData.Volume },
						{ "looping", AudioSourceData.Looping },
						{ "playOnStart", AudioSourceData.PlayOnStart },
						{ "spatialized", AudioSourceData.Spatialized }
					});
				}
				if (AudioListener->has_value())
					Record["audioListener"] = Json::object({ { "primary", AudioListener->value().IsPrimary } });
				if (Script->has_value())
				{
					Record["script"] = Json::object({
						{ "asset", Script->value().ScriptAsset.ToString() },
						{ "enabled", Script->value().Enabled }
					});
				}
				Record["parent"] = Parent->has_value()
					? Json((**Parent).GetUUID().ToString())
					: Json(nullptr);
				Document["entities"].push_back(std::move(Record));
			}

			return Document.dump(2) + "\n";
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(MakeError(
				SceneSerializationErrorCode::SceneOperationFailed,
				std::string("Scene serialization failed: ") + Exception.what()));
		}
	}

	std::expected<void, SceneSerializationError> SceneSerializer::Deserialize(std::string_view Data, Scene& Destination)
	{
		try
		{
			const Json Document = Json::parse(Data.begin(), Data.end());
			if (!Document.is_object())
				return std::unexpected(MakeError(SceneSerializationErrorCode::InvalidDocument, "Scene document root must be an object"));

			const auto Format = Document.find("format");
			if (Format == Document.end() || !Format->is_string())
				return std::unexpected(MakeError(SceneSerializationErrorCode::InvalidDocument, "Scene document is missing its format identifier"));
			if (Format->get<std::string>() != SceneFormatName)
				return std::unexpected(MakeError(SceneSerializationErrorCode::UnsupportedFormat, "Document is not a PulseForge scene"));

			const auto Version = Document.find("version");
			if (Version == Document.end() || (!Version->is_number_integer() && !Version->is_number_unsigned()))
				return std::unexpected(MakeError(SceneSerializationErrorCode::InvalidDocument, "Scene document is missing a numeric format version"));
			const bool SupportedVersion = Version->is_number_unsigned()
				? Version->get<uint64_t>() >= static_cast<uint64_t>(MinimumSupportedSceneFormatVersion) &&
					Version->get<uint64_t>() <= static_cast<uint64_t>(SceneFormatVersion)
				: Version->get<int64_t>() >= MinimumSupportedSceneFormatVersion &&
					Version->get<int64_t>() <= SceneFormatVersion;
			if (!SupportedVersion)
				return std::unexpected(MakeError(SceneSerializationErrorCode::UnsupportedVersion, "Scene document version is not supported"));

			const auto SerializedEntities = Document.find("entities");
			if (SerializedEntities == Document.end() || !SerializedEntities->is_array())
				return std::unexpected(MakeError(SceneSerializationErrorCode::InvalidDocument, "Scene document entities must be an array"));

			struct PendingParent
			{
				Entity Child;
				std::optional<UUID> Parent;
			};

			Scene Staging;
			std::vector<PendingParent> PendingParents;
			PendingParents.reserve(SerializedEntities->size());
			std::unordered_set<UUID, UUIDHash> EntityIdentifiers;
			EntityIdentifiers.reserve(SerializedEntities->size());
			for (const Json& SerializedEntity : *SerializedEntities)
			{
				if (!SerializedEntity.is_object())
					return std::unexpected(MakeError(SceneSerializationErrorCode::InvalidEntityData, "Each scene entity must be an object"));

				const auto UUIDValue = SerializedEntity.find("uuid");
				const auto Tag = SerializedEntity.find("tag");
				const auto Transform = SerializedEntity.find("transform");
				const auto Parent = SerializedEntity.find("parent");
				if (UUIDValue == SerializedEntity.end() || !UUIDValue->is_string() ||
					Tag == SerializedEntity.end() || !Tag->is_object() ||
					Transform == SerializedEntity.end() || !Transform->is_object() ||
					Parent == SerializedEntity.end())
				{
					return std::unexpected(MakeError(
						SceneSerializationErrorCode::InvalidEntityData,
						"Entity requires UUID, tag, transform, and parent fields"));
				}

				const auto ParsedUUID = UUID::Parse(UUIDValue->get<std::string>());
				if (!ParsedUUID)
					return std::unexpected(MakeError(SceneSerializationErrorCode::InvalidEntityData, ParsedUUID.error().Message));
				if (ParsedUUID->IsNil())
					return std::unexpected(MakeError(SceneSerializationErrorCode::InvalidEntityData, "Entity UUID must not be nil"));
				if (!EntityIdentifiers.insert(*ParsedUUID).second)
					return std::unexpected(MakeError(SceneSerializationErrorCode::InvalidEntityData, "Scene document contains a duplicate entity UUID"));

				const auto Name = Tag->find("name");
				if (Name == Tag->end() || !Name->is_string())
					return std::unexpected(MakeError(SceneSerializationErrorCode::InvalidEntityData, "Entity tag requires a string name"));

				const auto Translation = Transform->find("translation");
				const auto Rotation = Transform->find("rotation");
				const auto Scale = Transform->find("scale");
				if (Translation == Transform->end() || Rotation == Transform->end() || Scale == Transform->end())
				{
					return std::unexpected(MakeError(
						SceneSerializationErrorCode::InvalidEntityData,
						"Entity transform is missing translation, rotation, or scale"));
				}

				std::array<float, 3> TranslationValues{};
				std::array<float, 4> RotationValues{};
				std::array<float, 3> ScaleValues{};
				if (!ReadFiniteFloats(*Translation, TranslationValues) ||
					!ReadFiniteFloats(*Rotation, RotationValues) ||
					!ReadFiniteFloats(*Scale, ScaleValues))
				{
					return std::unexpected(MakeError(
						SceneSerializationErrorCode::InvalidEntityData,
						"Transform vectors/quaternion must contain the expected number of finite numeric values"));
				}

				std::optional<UUID> ParentIdentifier;
				if (!Parent->is_null())
				{
					if (!Parent->is_string())
						return std::unexpected(MakeError(SceneSerializationErrorCode::InvalidEntityData, "Entity parent must be a UUID string or null"));
					const auto ParsedParent = UUID::Parse(Parent->get<std::string>());
					if (!ParsedParent)
						return std::unexpected(MakeError(SceneSerializationErrorCode::InvalidEntityData, ParsedParent.error().Message));
					ParentIdentifier = ParsedParent.value();
				}

				std::optional<CameraComponent> CameraData;
				const auto SerializedCamera = SerializedEntity.find("camera");
				if (SerializedCamera != SerializedEntity.end())
				{
					if (!SerializedCamera->is_object())
						return std::unexpected(MakeError(SceneSerializationErrorCode::InvalidEntityData, "Entity camera must be an object"));

					const auto VerticalFov = SerializedCamera->find("verticalFovRadians");
					const auto NearClip = SerializedCamera->find("nearClipPlane");
					const auto FarClip = SerializedCamera->find("farClipPlane");
					const auto Primary = SerializedCamera->find("primary");
					if (VerticalFov == SerializedCamera->end() || NearClip == SerializedCamera->end() ||
						FarClip == SerializedCamera->end() || !VerticalFov->is_number() ||
						!NearClip->is_number() || !FarClip->is_number() ||
						(Primary != SerializedCamera->end() && !Primary->is_boolean()))
					{
						return std::unexpected(MakeError(
							SceneSerializationErrorCode::InvalidEntityData,
							"Camera requires numeric field of view and clip planes, and an optional boolean primary field"));
					}

					CameraData = CameraComponent{
						VerticalFov->get<float>(),
						NearClip->get<float>(),
						FarClip->get<float>(),
						Primary != SerializedCamera->end() && Primary->get<bool>() };
					if (auto CameraValidation = CameraData->Validate(); !CameraValidation)
						return std::unexpected(MakeError(SceneSerializationErrorCode::InvalidEntityData, CameraValidation.error().Message));
				}

				std::optional<DirectionalLightComponent> DirectionalLightData;
				const auto SerializedDirectionalLight = SerializedEntity.find("directionalLight");
				if (SerializedDirectionalLight != SerializedEntity.end())
				{
					if (!SerializedDirectionalLight->is_object())
						return std::unexpected(MakeError(SceneSerializationErrorCode::InvalidEntityData,
							"Entity directionalLight must be an object"));
					const auto Color = SerializedDirectionalLight->find("color");
					const auto Intensity = SerializedDirectionalLight->find("intensity");
					std::array<float, 3> ColorValues{};
					if (Color == SerializedDirectionalLight->end() || Intensity == SerializedDirectionalLight->end() ||
						!ReadFiniteFloats(*Color, ColorValues) || !Intensity->is_number())
						return std::unexpected(MakeError(SceneSerializationErrorCode::InvalidEntityData,
							"Directional light requires finite RGB color and numeric intensity"));
					DirectionalLightComponent Light;
					Light.Color = { ColorValues[0], ColorValues[1], ColorValues[2] };
					Light.Intensity = Intensity->get<float>();
					const auto ReadOptionalFloat = [&SerializedDirectionalLight](const char* Key, float& Target)
					{
						const auto Value = SerializedDirectionalLight->find(Key);
						if (Value == SerializedDirectionalLight->end())
							return true;
						if (!Value->is_number())
							return false;
						Target = Value->get<float>();
						return std::isfinite(Target);
					};
					const auto CastShadows = SerializedDirectionalLight->find("castShadows");
					if ((CastShadows != SerializedDirectionalLight->end() && !CastShadows->is_boolean()) ||
						!ReadOptionalFloat("shadowDistance", Light.ShadowDistance) ||
						!ReadOptionalFloat("shadowBias", Light.ShadowBias) ||
						!ReadOptionalFloat("shadowNormalBias", Light.ShadowNormalBias) ||
						!ReadOptionalFloat("shadowSoftness", Light.ShadowSoftness))
						return std::unexpected(MakeError(SceneSerializationErrorCode::InvalidEntityData,
							"Directional-light shadow settings contain invalid values"));
					if (CastShadows != SerializedDirectionalLight->end())
						Light.CastShadows = CastShadows->get<bool>();
					DirectionalLightData = Light;
					if (auto Validation = DirectionalLightData->Validate(); !Validation)
						return std::unexpected(MakeError(SceneSerializationErrorCode::InvalidEntityData, Validation.error()));
				}

				std::optional<EnvironmentLightComponent> EnvironmentLightData;
				const auto SerializedEnvironmentLight = SerializedEntity.find("environmentLight");
				if (SerializedEnvironmentLight != SerializedEntity.end())
				{
					if (!SerializedEnvironmentLight->is_object())
						return std::unexpected(MakeError(SceneSerializationErrorCode::InvalidEntityData,
							"Entity environmentLight must be an object"));
					const auto HdrImage = SerializedEnvironmentLight->find("hdrImage");
					const auto Intensity = SerializedEnvironmentLight->find("intensity");
					if (HdrImage == SerializedEnvironmentLight->end() || !HdrImage->is_string() ||
						Intensity == SerializedEnvironmentLight->end() || !Intensity->is_number())
						return std::unexpected(MakeError(SceneSerializationErrorCode::InvalidEntityData,
							"Environment light requires an HDR image UUID and numeric intensity"));
					const auto ParsedImage = UUID::Parse(HdrImage->get<std::string>());
					if (!ParsedImage || ParsedImage->IsNil())
						return std::unexpected(MakeError(SceneSerializationErrorCode::InvalidEntityData,
							ParsedImage ? "Environment HDR image UUID must not be nil" : ParsedImage.error().Message));
					EnvironmentLightData = EnvironmentLightComponent{ *ParsedImage, Intensity->get<float>() };
					if (auto Validation = EnvironmentLightData->Validate(); !Validation)
						return std::unexpected(MakeError(SceneSerializationErrorCode::InvalidEntityData, Validation.error()));
				}

				std::optional<PointLightComponent> PointLightData;
				const auto SerializedPointLight = SerializedEntity.find("pointLight");
				if (SerializedPointLight != SerializedEntity.end())
				{
					if (!SerializedPointLight->is_object())
						return std::unexpected(MakeError(SceneSerializationErrorCode::InvalidEntityData, "Entity pointLight must be an object"));
					const auto Color = SerializedPointLight->find("color");
					const auto Intensity = SerializedPointLight->find("intensity");
					const auto Range = SerializedPointLight->find("range");
					std::array<float, 3> ColorValues{};
					if (Color == SerializedPointLight->end() || Intensity == SerializedPointLight->end() ||
						Range == SerializedPointLight->end() || !ReadFiniteFloats(*Color, ColorValues) ||
						!Intensity->is_number() || !Range->is_number())
						return std::unexpected(MakeError(SceneSerializationErrorCode::InvalidEntityData,
							"Point light requires finite RGB color, intensity, and range"));
					PointLightData = PointLightComponent{ { ColorValues[0], ColorValues[1], ColorValues[2] },
						Intensity->get<float>(), Range->get<float>() };
					if (auto Validation = PointLightData->Validate(); !Validation)
						return std::unexpected(MakeError(SceneSerializationErrorCode::InvalidEntityData, Validation.error()));
				}

				std::optional<SpotLightComponent> SpotLightData;
				const auto SerializedSpotLight = SerializedEntity.find("spotLight");
				if (SerializedSpotLight != SerializedEntity.end())
				{
					if (!SerializedSpotLight->is_object())
						return std::unexpected(MakeError(SceneSerializationErrorCode::InvalidEntityData, "Entity spotLight must be an object"));
					const auto Color = SerializedSpotLight->find("color");
					const auto Intensity = SerializedSpotLight->find("intensity");
					const auto Range = SerializedSpotLight->find("range");
					const auto Inner = SerializedSpotLight->find("innerConeAngleDegrees");
					const auto Outer = SerializedSpotLight->find("outerConeAngleDegrees");
					std::array<float, 3> ColorValues{};
					if (Color == SerializedSpotLight->end() || Intensity == SerializedSpotLight->end() ||
						Range == SerializedSpotLight->end() || Inner == SerializedSpotLight->end() || Outer == SerializedSpotLight->end() ||
						!ReadFiniteFloats(*Color, ColorValues) || !Intensity->is_number() || !Range->is_number() ||
						!Inner->is_number() || !Outer->is_number())
						return std::unexpected(MakeError(SceneSerializationErrorCode::InvalidEntityData,
							"Spot light requires finite RGB color, intensity, range, and cone half-angles in degrees"));
					SpotLightData = SpotLightComponent{ { ColorValues[0], ColorValues[1], ColorValues[2] },
						Intensity->get<float>(), Range->get<float>(), Inner->get<float>(), Outer->get<float>() };
					if (auto Validation = SpotLightData->Validate(); !Validation)
						return std::unexpected(MakeError(SceneSerializationErrorCode::InvalidEntityData, Validation.error()));
				}

				std::optional<MeshRendererComponent> MeshRendererData;
				const auto SerializedMeshRenderer = SerializedEntity.find("meshRenderer");
				if (SerializedMeshRenderer != SerializedEntity.end())
				{
					if (!SerializedMeshRenderer->is_object())
						return std::unexpected(MakeError(
							SceneSerializationErrorCode::InvalidEntityData,
							"Entity meshRenderer must be an object"));

					const auto MeshAsset = SerializedMeshRenderer->find("meshAsset");
					if (MeshAsset == SerializedMeshRenderer->end() || !MeshAsset->is_string())
						return std::unexpected(MakeError(
							SceneSerializationErrorCode::InvalidEntityData,
							"Mesh renderer requires a meshAsset UUID string"));

					const auto ParsedMeshAsset = UUID::Parse(MeshAsset->get<std::string>());
					if (!ParsedMeshAsset || ParsedMeshAsset->IsNil())
						return std::unexpected(MakeError(
							SceneSerializationErrorCode::InvalidEntityData,
							ParsedMeshAsset ? "Mesh renderer asset UUID must not be nil" : ParsedMeshAsset.error().Message));

					std::optional<AssetID> MaterialAsset;
					const auto SerializedMaterialAsset = SerializedMeshRenderer->find("materialAsset");
					if (SerializedMaterialAsset != SerializedMeshRenderer->end())
					{
						if (!SerializedMaterialAsset->is_string())
							return std::unexpected(MakeError(
								SceneSerializationErrorCode::InvalidEntityData,
								"Mesh renderer materialAsset must be a UUID string when present"));
						const auto ParsedMaterialAsset = UUID::Parse(SerializedMaterialAsset->get<std::string>());
						if (!ParsedMaterialAsset || ParsedMaterialAsset->IsNil())
							return std::unexpected(MakeError(
								SceneSerializationErrorCode::InvalidEntityData,
								ParsedMaterialAsset ? "Mesh renderer material UUID must not be nil" : ParsedMaterialAsset.error().Message));
						MaterialAsset = *ParsedMaterialAsset;
					}

					MeshRendererData = MeshRendererComponent{ *ParsedMeshAsset, MaterialAsset };
				}

				std::optional<RigidbodyComponent> RigidbodyData;
				const auto SerializedRigidbody = SerializedEntity.find("rigidbody");
				if (SerializedRigidbody != SerializedEntity.end())
				{
					if (!SerializedRigidbody->is_object())
						return std::unexpected(MakeError(SceneSerializationErrorCode::InvalidEntityData, "Entity rigidbody must be an object"));

					const auto MotionType = SerializedRigidbody->find("motionType");
					const auto Mass = SerializedRigidbody->find("mass");
					const auto Friction = SerializedRigidbody->find("friction");
					const auto Restitution = SerializedRigidbody->find("restitution");
					const auto AllowSleeping = SerializedRigidbody->find("allowSleeping");
					if (MotionType == SerializedRigidbody->end() || !MotionType->is_string() ||
						Mass == SerializedRigidbody->end() || !Mass->is_number() ||
						Friction == SerializedRigidbody->end() || !Friction->is_number() ||
						Restitution == SerializedRigidbody->end() || !Restitution->is_number() ||
						AllowSleeping == SerializedRigidbody->end() || !AllowSleeping->is_boolean())
					{
						return std::unexpected(MakeError(
							SceneSerializationErrorCode::InvalidEntityData,
							"Rigidbody requires motionType, mass, friction, restitution, and allowSleeping fields"));
					}

					const std::string MotionName = MotionType->get<std::string>();
					if (MotionName != "static" && MotionName != "dynamic")
						return std::unexpected(MakeError(SceneSerializationErrorCode::InvalidEntityData, "Rigidbody motionType must be static or dynamic"));
					RigidbodyData = RigidbodyComponent{
						MotionName == "static" ? RigidbodyMotionType::Static : RigidbodyMotionType::Dynamic,
						Mass->get<float>(),
						Friction->get<float>(),
						Restitution->get<float>(),
						AllowSleeping->get<bool>() };
					if (auto Validation = RigidbodyData->Validate(); !Validation)
						return std::unexpected(MakeError(SceneSerializationErrorCode::InvalidEntityData, Validation.error().Message));
				}

				std::optional<BoxColliderComponent> BoxColliderData;
				const auto SerializedBoxCollider = SerializedEntity.find("boxCollider");
				if (SerializedBoxCollider != SerializedEntity.end())
				{
					if (!SerializedBoxCollider->is_object())
						return std::unexpected(MakeError(SceneSerializationErrorCode::InvalidEntityData, "Entity boxCollider must be an object"));
					const auto HalfExtents = SerializedBoxCollider->find("halfExtents");
					std::array<float, 3> HalfExtentValues{};
					if (HalfExtents == SerializedBoxCollider->end() || !ReadFiniteFloats(*HalfExtents, HalfExtentValues))
						return std::unexpected(MakeError(
							SceneSerializationErrorCode::InvalidEntityData,
							"Box collider halfExtents must contain three finite numbers"));
					BoxColliderData = BoxColliderComponent{ { HalfExtentValues[0], HalfExtentValues[1], HalfExtentValues[2] } };
					if (auto Validation = BoxColliderData->Validate(); !Validation)
						return std::unexpected(MakeError(SceneSerializationErrorCode::InvalidEntityData, Validation.error().Message));
				}

				std::optional<AudioSourceComponent> AudioSourceData;
				const auto SerializedAudioSource = SerializedEntity.find("audioSource");
				if (SerializedAudioSource != SerializedEntity.end())
				{
					if (!SerializedAudioSource->is_object())
						return std::unexpected(MakeError(SceneSerializationErrorCode::InvalidEntityData, "Entity audioSource must be an object"));
					const auto Asset = SerializedAudioSource->find("asset");
					const auto Volume = SerializedAudioSource->find("volume");
					const auto Looping = SerializedAudioSource->find("looping");
					const auto PlayOnStart = SerializedAudioSource->find("playOnStart");
					const auto Spatialized = SerializedAudioSource->find("spatialized");
					if (Asset == SerializedAudioSource->end() || !Asset->is_string() ||
						Volume == SerializedAudioSource->end() || !Volume->is_number() ||
						Looping == SerializedAudioSource->end() || !Looping->is_boolean() ||
						PlayOnStart == SerializedAudioSource->end() || !PlayOnStart->is_boolean() ||
						Spatialized == SerializedAudioSource->end() || !Spatialized->is_boolean())
					{
						return std::unexpected(MakeError(
							SceneSerializationErrorCode::InvalidEntityData,
							"Audio source requires asset, volume, looping, playOnStart, and spatialized fields"));
					}

					const auto ParsedAsset = UUID::Parse(Asset->get<std::string>());
					if (!ParsedAsset || ParsedAsset->IsNil())
						return std::unexpected(MakeError(
							SceneSerializationErrorCode::InvalidEntityData,
							ParsedAsset ? "Audio source asset UUID must not be nil" : ParsedAsset.error().Message));

					AudioSourceData = AudioSourceComponent{
						*ParsedAsset,
						Volume->get<float>(),
						Looping->get<bool>(),
						PlayOnStart->get<bool>(),
						Spatialized->get<bool>() };
					if (auto Validation = AudioSourceData->Validate(); !Validation)
						return std::unexpected(MakeError(SceneSerializationErrorCode::InvalidEntityData, Validation.error().Message));
				}

				std::optional<AudioListenerComponent> AudioListenerData;
				const auto SerializedAudioListener = SerializedEntity.find("audioListener");
				if (SerializedAudioListener != SerializedEntity.end())
				{
					if (!SerializedAudioListener->is_object())
						return std::unexpected(MakeError(SceneSerializationErrorCode::InvalidEntityData, "Entity audioListener must be an object"));
					const auto Primary = SerializedAudioListener->find("primary");
					if (Primary == SerializedAudioListener->end() || !Primary->is_boolean())
						return std::unexpected(MakeError(
							SceneSerializationErrorCode::InvalidEntityData,
							"Audio listener requires a boolean primary field"));
					AudioListenerData = AudioListenerComponent{ Primary->get<bool>() };
				}

				std::optional<ScriptComponent> ScriptData;
				const auto SerializedScript = SerializedEntity.find("script");
				if (SerializedScript != SerializedEntity.end())
				{
					if (!SerializedScript->is_object())
						return std::unexpected(MakeError(SceneSerializationErrorCode::InvalidEntityData, "Entity script must be an object"));
					const auto Asset = SerializedScript->find("asset");
					const auto Enabled = SerializedScript->find("enabled");
					if (Asset == SerializedScript->end() || !Asset->is_string() ||
						Enabled == SerializedScript->end() || !Enabled->is_boolean())
					{
						return std::unexpected(MakeError(
							SceneSerializationErrorCode::InvalidEntityData,
							"Script component requires an asset UUID and boolean enabled field"));
					}
					const auto ParsedAsset = UUID::Parse(Asset->get<std::string>());
					if (!ParsedAsset || ParsedAsset->IsNil())
						return std::unexpected(MakeError(
							SceneSerializationErrorCode::InvalidEntityData,
							ParsedAsset ? "Script asset UUID must not be nil" : ParsedAsset.error().Message));
					ScriptData = ScriptComponent{ *ParsedAsset, Enabled->get<bool>() };
					if (auto Validation = ScriptData->Validate(); !Validation)
						return std::unexpected(MakeError(SceneSerializationErrorCode::InvalidEntityData, Validation.error().Message));
				}

				auto Created = Staging.CreateEntityWithUUID(ParsedUUID.value(), Name->get<std::string>());
				if (!Created)
					return std::unexpected(SceneOperationError(Created.error()));

				TransformComponent ComponentTransform;
				ComponentTransform.Translation = { TranslationValues[0], TranslationValues[1], TranslationValues[2] };
				ComponentTransform.Rotation = glm::quat(RotationValues[0], RotationValues[1], RotationValues[2], RotationValues[3]);
				ComponentTransform.Scale = { ScaleValues[0], ScaleValues[1], ScaleValues[2] };
				if (auto TransformResult = Created->SetTransform(ComponentTransform); !TransformResult)
					return std::unexpected(SceneOperationError(TransformResult.error()));
				if (CameraData)
				{
					if (auto CameraResult = Created->SetCamera(*CameraData); !CameraResult)
						return std::unexpected(SceneOperationError(CameraResult.error()));
				}
				if (DirectionalLightData)
				{
					if (auto LightResult = Created->SetDirectionalLight(*DirectionalLightData); !LightResult)
						return std::unexpected(SceneOperationError(LightResult.error()));
				}
				if (EnvironmentLightData)
				{
					if (auto EnvironmentResult = Created->SetEnvironmentLight(*EnvironmentLightData); !EnvironmentResult)
						return std::unexpected(SceneOperationError(EnvironmentResult.error()));
				}
				if (PointLightData)
					if (auto LightResult = Created->SetPointLight(*PointLightData); !LightResult)
						return std::unexpected(SceneOperationError(LightResult.error()));
				if (SpotLightData)
					if (auto LightResult = Created->SetSpotLight(*SpotLightData); !LightResult)
						return std::unexpected(SceneOperationError(LightResult.error()));
				if (MeshRendererData)
				{
					if (auto MeshRendererResult = Created->SetMeshRenderer(*MeshRendererData); !MeshRendererResult)
						return std::unexpected(SceneOperationError(MeshRendererResult.error()));
				}
				if (RigidbodyData)
				{
					if (auto RigidbodyResult = Created->SetRigidbody(*RigidbodyData); !RigidbodyResult)
						return std::unexpected(SceneOperationError(RigidbodyResult.error()));
				}
				if (BoxColliderData)
				{
					if (auto ColliderResult = Created->SetBoxCollider(*BoxColliderData); !ColliderResult)
						return std::unexpected(SceneOperationError(ColliderResult.error()));
				}
				if (AudioSourceData)
				{
					if (auto AudioSourceResult = Created->SetAudioSource(*AudioSourceData); !AudioSourceResult)
						return std::unexpected(SceneOperationError(AudioSourceResult.error()));
				}
				if (AudioListenerData)
				{
					if (auto AudioListenerResult = Created->SetAudioListener(*AudioListenerData); !AudioListenerResult)
						return std::unexpected(SceneOperationError(AudioListenerResult.error()));
				}
				if (ScriptData)
				{
					if (auto ScriptResult = Created->SetScript(*ScriptData); !ScriptResult)
						return std::unexpected(SceneOperationError(ScriptResult.error()));
				}

				PendingParents.push_back({ *Created, ParentIdentifier });
			}

			for (const PendingParent& Relationship : PendingParents)
			{
				if (!Relationship.Parent)
					continue;
				const auto Parent = Staging.FindEntity(*Relationship.Parent);
				if (!Parent)
				{
					return std::unexpected(MakeError(
						SceneSerializationErrorCode::MissingParent,
						"Scene entity references a parent UUID that does not exist"));
				}
				if (auto ParentResult = Relationship.Child.SetParent(*Parent); !ParentResult)
					return std::unexpected(SceneOperationError(ParentResult.error()));
			}

			Destination.m_Storage.swap(Staging.m_Storage);
			return {};
		}
		catch (const nlohmann::json::exception& Exception)
		{
			return std::unexpected(MakeError(
				SceneSerializationErrorCode::InvalidDocument,
				std::string("Scene JSON is invalid: ") + Exception.what()));
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(MakeError(
				SceneSerializationErrorCode::InvalidDocument,
				std::string("Scene deserialization failed: ") + Exception.what()));
		}
	}

	std::expected<std::unique_ptr<Scene>, SceneSerializationError> SceneSerializer::Clone(const Scene& Source)
	{
		auto Serialized = Serialize(Source);
		if (!Serialized)
			return std::unexpected(Serialized.error());

		auto Copy = std::make_unique<Scene>();
		if (auto Result = Deserialize(*Serialized, *Copy); !Result)
			return std::unexpected(Result.error());

		return Copy;
	}

	std::expected<void, SceneSerializationError> SceneSerializer::SaveToFile(
		const Scene& Source,
		const std::filesystem::path& Path)
	{
		if (Path.empty())
			return std::unexpected(MakeError(SceneSerializationErrorCode::FileOpenFailed, "Scene file path must not be empty"));

		const auto Serialized = Serialize(Source);
		if (!Serialized)
			return std::unexpected(Serialized.error());

		try
		{
			const auto TemporaryIdentifier = UUID::Generate();
			if (!TemporaryIdentifier)
				return std::unexpected(MakeError(SceneSerializationErrorCode::SceneOperationFailed, TemporaryIdentifier.error().Message));

			std::filesystem::path TemporaryPath = Path;
			TemporaryPath += "." + TemporaryIdentifier->ToString() + ".tmp";
			TemporaryFileCleanup Cleanup{ TemporaryPath };

			std::ofstream Output(TemporaryPath, std::ios::binary | std::ios::trunc);
			if (!Output.is_open())
			{
				return std::unexpected(MakeError(
					SceneSerializationErrorCode::FileOpenFailed,
					"Could not open scene file for writing: " + PathForMessage(Path)));
			}
			if (Serialized->size() > static_cast<size_t>(std::numeric_limits<std::streamsize>::max()))
			{
				return std::unexpected(MakeError(
					SceneSerializationErrorCode::FileWriteFailed,
					"Serialized scene is too large to write: " + PathForMessage(Path)));
			}

			Output.write(Serialized->data(), static_cast<std::streamsize>(Serialized->size()));
			Output.flush();
			if (!Output)
			{
				return std::unexpected(MakeError(
					SceneSerializationErrorCode::FileWriteFailed,
					"Could not write the complete scene document: " + PathForMessage(Path)));
			}
			Output.close();
			if (Output.fail())
			{
				return std::unexpected(MakeError(
					SceneSerializationErrorCode::FileWriteFailed,
					"Could not finish writing the scene document: " + PathForMessage(Path)));
			}

			std::error_code ReplaceError;
			std::filesystem::rename(TemporaryPath, Path, ReplaceError);
			if (ReplaceError)
			{
				return std::unexpected(MakeError(
					SceneSerializationErrorCode::FileReplaceFailed,
					"Could not replace scene file '" + PathForMessage(Path) + "': " + ReplaceError.message()));
			}
			return {};
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(MakeError(
				SceneSerializationErrorCode::FileWriteFailed,
				std::string("Scene file save failed: ") + Exception.what()));
		}
	}

	std::expected<void, SceneSerializationError> SceneSerializer::LoadFromFile(
		const std::filesystem::path& Path,
		Scene& Destination)
	{
		if (Path.empty())
			return std::unexpected(MakeError(SceneSerializationErrorCode::FileOpenFailed, "Scene file path must not be empty"));

		try
		{
			std::ifstream Input(Path, std::ios::binary);
			if (!Input.is_open())
			{
				return std::unexpected(MakeError(
					SceneSerializationErrorCode::FileOpenFailed,
					"Could not open scene file for reading: " + PathForMessage(Path)));
			}

			std::string Data{ std::istreambuf_iterator<char>(Input), std::istreambuf_iterator<char>() };
			if (Input.bad())
			{
				return std::unexpected(MakeError(
					SceneSerializationErrorCode::FileReadFailed,
					"Could not read the complete scene file: " + PathForMessage(Path)));
			}
			return Deserialize(Data, Destination);
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(MakeError(
				SceneSerializationErrorCode::FileReadFailed,
				std::string("Scene file load failed: ") + Exception.what()));
		}
	}
}
