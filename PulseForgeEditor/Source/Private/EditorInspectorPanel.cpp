#include "Editor/EditorInspectorPanel.h"

#include "Editor/EditorAssetSelector.h"
#include "Editor/EditorAssetSelectorPolicy.h"
#include "Editor/EditorHierarchyPanel.h"
#include "Editor/EditorHierarchyPanelPolicy.h"
#include "Editor/EditorSceneWorkspaceState.h"
#include "Editor/EditorStyle.h"

#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <expected>
#include <functional>
#include <initializer_list>

namespace PulseForgeEditor
{
	namespace
	{
		std::string PathToUtf8(const std::filesystem::path& Path)
		{
			const std::u8string Encoded = Path.u8string();
			return std::string(reinterpret_cast<const char*>(Encoded.data()), Encoded.size());
		}

		PulseForge::SceneError ToSceneError(const PulseForge::PropertyAccessError& Error)
		{
			const PulseForge::SceneErrorCode Code = Error.Code == PulseForge::PropertyAccessErrorCode::MissingComponent
														? PulseForge::SceneErrorCode::MissingComponent
														: PulseForge::SceneErrorCode::StorageFailure;
			return { Code, Error.Message };
		}
	} // namespace

	struct EditorInspectorPanel::Context
	{
		PulseForge::Scene* SceneValue;
		EditorSceneWorkspaceState& Workspace;
		EditorStyle& Style;
		EditorAssetSelector& AssetSelector;
		const std::vector<PulseForge::AssetRecord>& Assets;
		bool ProjectAvailable;
		EditorInspectorPanelActions& Actions;
	};

	void EditorInspectorPanel::DrawContents(PulseForge::Scene* SceneValue, EditorSceneWorkspaceState& Workspace, EditorStyle& Style,
		EditorAssetSelector& AssetSelector, const std::vector<PulseForge::AssetRecord>& Assets, bool ProjectAvailable,
		bool FinishCurrentEdit, EditorInspectorPanelActions& Actions)
	{
		Context Inspector{ SceneValue, Workspace, Style, AssetSelector, Assets, ProjectAvailable, Actions };
		if (FinishCurrentEdit) Actions.FinishInspectorSceneEdit();

		const auto& SelectedEntity = Workspace.Selection().GetPrimary();
		if (!SceneValue || !SelectedEntity) Style.EmptyState(EditorIcon::Entity, "Select an entity in the Hierarchy.");
		else if (Workspace.Selection().GetEntities().size() > 1)
		{
			ImGui::Text("%zu entities selected", Workspace.Selection().GetEntities().size());
			ImGui::Spacing();
			Style.TextMuted("The Inspector shows the active entity. Multi-object property editing is not available yet.");
		}
		else if (const auto Entity = SceneValue->FindEntity(*SelectedEntity))
		{
			auto Tag = Entity->GetTag();
			if (Tag)
			{
				if (Style.BeginToolbar("InspectorEntityHeader", 74.0f))
				{
					Style.IconText(GetEditorEntityIcon(*Entity), {});
					ImGui::SameLine();
					const float NameWidth = (std::max)(ImGui::GetContentRegionAvail().x, 1.0f);
					ImGui::SetNextItemWidth(NameWidth);
					bool NameChanged = false;
					{
						[[maybe_unused]] const auto EmphasisFont = Style.PushEmphasisFont();
						if (m_FocusEntityNameNextFrame)
						{
							ImGui::SetKeyboardFocusHere();
							m_FocusEntityNameNextFrame = false;
						}
						NameChanged = ImGui::InputText("##EntityName", &Tag->Name);
					}
					if (NameChanged)
					{
						const auto Updated =
							Actions.RunInspectorSceneEdit("Rename Entity", [Entity = *Entity, Tag = *Tag] { return Entity.SetTag(Tag); });
						if (!Updated) Actions.ReportInspectorError("Entity name update failed: " + Updated.error());
					}
					ImGui::SetCursorPosX(ImGui::GetStyle().WindowPadding.x + 29.0f);
					Style.TextMuted(Entity->GetUUID().ToString());
				}
				Style.EndToolbar();
				ImGui::Spacing();
			}

			if (const auto Transform = Entity->GetTransform())
			{
				ImGui::Spacing();
				if (Style.SectionHeader(EditorIcon::Translate, "Transform", true))
				{
					Style.BeginComponentBody("TransformComponentBody");
					if (Style.BeginPropertyTable("TransformProperties"))
					{
						if (Style.BeginPropertyRow("Position"))
							DrawProperty(Inspector, *Entity, PulseForge::PropertyComponentIds::Transform,
								PulseForge::PropertyIds::Translation,
								[](PulseForge::PropertyValue& Value)
								{
									auto* Translation = std::get_if<glm::vec3>(&Value);
									return Translation &&
										   ImGui::DragFloat3("##TransformPosition", &Translation->x, 0.05f, 0.0f, 0.0f, "%.3f");
								});
						if (Style.BeginPropertyRow("Rotation"))
							DrawProperty(Inspector, *Entity, PulseForge::PropertyComponentIds::Transform, PulseForge::PropertyIds::Rotation,
								[](PulseForge::PropertyValue& Value)
								{
									auto* Rotation = std::get_if<glm::quat>(&Value);
									if (!Rotation) return false;
									const glm::vec3 Degrees = glm::degrees(glm::eulerAngles(*Rotation));
									float RotationDegrees[3]{ Degrees.x, Degrees.y, Degrees.z };
									if (!ImGui::DragFloat3("##TransformRotation", RotationDegrees, 0.5f, 0.0f, 0.0f, "%.1f")) return false;
									*Rotation =
										glm::quat(glm::radians(glm::vec3(RotationDegrees[0], RotationDegrees[1], RotationDegrees[2])));
									return true;
								});
						if (Style.BeginPropertyRow("Scale"))
							DrawProperty(Inspector, *Entity, PulseForge::PropertyComponentIds::Transform, PulseForge::PropertyIds::Scale,
								[](PulseForge::PropertyValue& Value)
								{
									auto* Scale = std::get_if<glm::vec3>(&Value);
									return Scale && ImGui::DragFloat3("##TransformScale", &Scale->x, 0.05f, 0.0f, 0.0f, "%.3f");
								});
						Style.EndPropertyTable();
					}
					Style.EndComponentBody();
				}
			}
			ImGui::Separator();
			DrawExistingComponents(Inspector, *Entity);
			DrawAddComponentControl(Inspector, *Entity);
		}
		else
		{
			Workspace.Selection().Clear();
			Style.TextMuted("The selected entity no longer exists.");
		}
	}

	template <typename Action>
	void EditorInspectorPanel::RecordComponentOperation(Context& Inspector, Action&& Operation, std::string_view Context)
	{
		const auto Result = Inspector.Actions.RunInspectorSceneEdit(std::string(Context), std::forward<Action>(Operation));
		if (!Result)
		{
			Inspector.Actions.ReportInspectorError(std::string(Context) + ": " + Result.error());
			return;
		}
	}

	void EditorInspectorPanel::DrawProperty(Context& Inspector, const PulseForge::Entity& Entity, std::string_view ComponentId,
		std::string_view PropertyId, const std::function<bool(PulseForge::PropertyValue&)>& DrawWidget)
	{
		auto Value = PulseForge::ReadProperty(Entity, ComponentId, PropertyId);
		if (!Value)
		{
			Inspector.Actions.ReportInspectorError(
				"Could not read Inspector property '" + std::string(PropertyId) + "': " + Value.error().Message);
			return;
		}
		if (!DrawWidget(*Value)) return;

		const auto Updated = Inspector.Actions.RunInspectorSceneEdit("Edit " + std::string(PropertyId),
			[Entity, ComponentId = std::string(ComponentId), PropertyId = std::string(PropertyId),
				Value = std::move(*Value)]() -> std::expected<void, PulseForge::SceneError>
			{
				const auto Result = PulseForge::WriteProperty(Entity, ComponentId, PropertyId, Value);
				if (!Result)
					return std::unexpected(PulseForge::SceneError{ PulseForge::SceneErrorCode::StorageFailure, Result.error().Message });
				return {};
			});
		if (!Updated) Inspector.Actions.ReportInspectorError("Inspector property update failed: " + Updated.error());
	}

	void EditorInspectorPanel::SetCameraComponent(
		Context& Inspector, const PulseForge::Entity& Target, const PulseForge::CameraComponent& Camera)
	{
		if (auto Validation = Camera.Validate(); !Validation)
		{
			Inspector.Actions.ReportInspectorError("Camera update failed: " + Validation.error().Message);
			return;
		}
		RecordComponentOperation(
			Inspector,
			[Scene = Inspector.SceneValue, Target, Camera]() -> std::expected<void, PulseForge::SceneError>
			{
				if (Camera.IsPrimary && Scene)
				{
					for (const PulseForge::Entity& Other : Scene->GetEntities())
					{
						if (Other.GetUUID() == Target.GetUUID()) continue;
						auto Existing = Other.GetCamera();
						if (!Existing) return std::unexpected(Existing.error());
						if (!Existing->has_value() || !Existing->value().IsPrimary) continue;
						PulseForge::CameraComponent Updated = Existing->value();
						Updated.IsPrimary = false;
						if (auto Result = Other.SetCamera(Updated); !Result) return std::unexpected(Result.error());
					}
				}
				return Target.SetCamera(Camera);
			},
			"Camera update failed");
	}

	void EditorInspectorPanel::SetAudioListenerComponent(
		Context& Inspector, const PulseForge::Entity& Target, const PulseForge::AudioListenerComponent& Listener)
	{
		RecordComponentOperation(
			Inspector,
			[Scene = Inspector.SceneValue, Target, Listener]() -> std::expected<void, PulseForge::SceneError>
			{
				if (Listener.IsPrimary && Scene)
				{
					for (const PulseForge::Entity& Other : Scene->GetEntities())
					{
						if (Other.GetUUID() == Target.GetUUID()) continue;
						auto Existing = Other.GetAudioListener();
						if (!Existing) return std::unexpected(Existing.error());
						if (!Existing->has_value() || !Existing->value().IsPrimary) continue;
						PulseForge::AudioListenerComponent Updated = Existing->value();
						Updated.IsPrimary = false;
						if (auto Result = Other.SetAudioListener(Updated); !Result) return std::unexpected(Result.error());
					}
				}
				return Target.SetAudioListener(Listener);
			},
			"Audio listener update failed");
	}

	bool EditorInspectorPanel::DrawAssetSelector(Context& Inspector, const char* Label, std::optional<PulseForge::AssetID>& Current,
		std::initializer_list<std::string_view> Extensions, bool AllowNone)
	{
		static const std::vector<PulseForge::AssetRecord> EmptyAssets;
		return Inspector.AssetSelector.Draw(
			Label, Current, Inspector.ProjectAvailable ? Inspector.Assets : EmptyAssets, Extensions, AllowNone);
	}

	bool EditorInspectorPanel::IsAssetExtension(
		const PulseForge::AssetRecord& Asset, std::initializer_list<std::string_view> Extensions) const
	{
		return PulseForgeEditor::AssetHasExtension(Asset, Extensions);
	}

	void EditorInspectorPanel::DrawAddComponentControl(Context& Inspector, const PulseForge::Entity& Entity)
	{
		if (Inspector.Style.FullWidthButton(PulseForgeEditor::EditorIcon::AddComponent, "Add Component")) ImGui::OpenPopup("Add Component");
		if (ImGui::BeginPopup("Add Component"))
		{
			const auto Camera = Entity.GetCamera();
			const auto DirectionalLight = Entity.GetDirectionalLight();
			const auto EnvironmentLight = Entity.GetEnvironmentLight();
			const auto PointLight = Entity.GetPointLight();
			const auto SpotLight = Entity.GetSpotLight();
			const auto Mesh = Entity.GetMeshRenderer();
			const auto Rigidbody = Entity.GetRigidbody();
			const auto Collider = Entity.GetBoxCollider();
			const auto AudioSource = Entity.GetAudioSource();
			const auto AudioListener = Entity.GetAudioListener();
			const auto Script = Entity.GetScript();
			if (!Camera || !DirectionalLight || !EnvironmentLight || !PointLight || !SpotLight || !Mesh || !Rigidbody || !Collider ||
				!AudioSource || !AudioListener || !Script)
				Inspector.Actions.ReportInspectorError("Could not inspect entity components.");

			if (Camera && !Camera->has_value() && Inspector.Style.MenuItem(PulseForgeEditor::EditorIcon::Camera, "Camera"))
			{
				PulseForge::CameraComponent Component;
				Component.IsPrimary = true;
				SetCameraComponent(Inspector, Entity, Component);
			}
			if (DirectionalLight && !DirectionalLight->has_value() &&
				Inspector.Style.MenuItem(PulseForgeEditor::EditorIcon::DirectionalLight, "Directional Light"))
				RecordComponentOperation(
					Inspector, [Entity] { return Entity.SetDirectionalLight(PulseForge::DirectionalLightComponent{}); },
					"Directional light component add failed");
			if (EnvironmentLight && !EnvironmentLight->has_value() && ImGui::BeginMenu("Environment Light"))
			{
				bool HasHdrImages = false;
				for (const PulseForge::AssetRecord& Asset : Inspector.Assets)
				{
					if (!IsAssetExtension(Asset, { ".hdr" })) continue;
					HasHdrImages = true;
					const std::string Path = PathToUtf8(Asset.ProjectRelativePath);
					if (ImGui::MenuItem(Path.c_str()))
					{
						PulseForge::EnvironmentLightComponent Component;
						Component.HdrImage = Asset.ID;
						RecordComponentOperation(
							Inspector, [Entity, Component] { return Entity.SetEnvironmentLight(Component); },
							"Environment light add failed");
						break;
					}
				}
				if (!HasHdrImages) ImGui::MenuItem("Import a Radiance HDR image first", nullptr, false, false);
				ImGui::EndMenu();
			}
			if (PointLight && !PointLight->has_value() && ImGui::MenuItem("Point Light"))
				RecordComponentOperation(
					Inspector, [Entity] { return Entity.SetPointLight(PulseForge::PointLightComponent{}); },
					"Point light component add failed");
			if (SpotLight && !SpotLight->has_value() && ImGui::MenuItem("Spot Light"))
				RecordComponentOperation(
					Inspector, [Entity] { return Entity.SetSpotLight(PulseForge::SpotLightComponent{}); },
					"Spot light component add failed");
			if (Rigidbody && !Rigidbody->has_value() && ImGui::MenuItem("Rigidbody"))
				RecordComponentOperation(
					Inspector, [Entity] { return Entity.SetRigidbody(PulseForge::RigidbodyComponent{}); },
					"Rigidbody component add failed");
			if (Collider && !Collider->has_value() && ImGui::MenuItem("Box Collider"))
				RecordComponentOperation(
					Inspector, [Entity] { return Entity.SetBoxCollider(PulseForge::BoxColliderComponent{}); }, "Box collider add failed");
			if (AudioListener && !AudioListener->has_value() && ImGui::MenuItem("Audio Listener"))
				SetAudioListenerComponent(Inspector, Entity, PulseForge::AudioListenerComponent{});

			if (Mesh && !Mesh->has_value() && ImGui::BeginMenu("Mesh Renderer"))
			{
				bool HasMeshAssets = false;
				for (const PulseForge::AssetRecord& Asset : Inspector.Assets)
				{
					if (!IsAssetExtension(Asset, { ".gltf", ".glb" })) continue;
					HasMeshAssets = true;
					const std::string Path = PathToUtf8(Asset.ProjectRelativePath);
					if (ImGui::MenuItem(Path.c_str()))
					{
						PulseForge::MeshRendererComponent Component;
						Component.MeshAsset = Asset.ID;
						RecordComponentOperation(
							Inspector, [Entity, Component] { return Entity.SetMeshRenderer(Component); }, "Mesh renderer add failed");
						break;
					}
				}
				if (!HasMeshAssets) ImGui::MenuItem("Import a glTF asset first", nullptr, false, false);
				ImGui::EndMenu();
			}

			if (AudioSource && !AudioSource->has_value() && ImGui::BeginMenu("Audio Source"))
			{
				bool HasAudioAssets = false;
				for (const PulseForge::AssetRecord& Asset : Inspector.Assets)
				{
					if (!IsAssetExtension(Asset, { ".wav", ".mp3", ".flac", ".ogg" })) continue;
					HasAudioAssets = true;
					const std::string Path = PathToUtf8(Asset.ProjectRelativePath);
					if (ImGui::MenuItem(Path.c_str()))
					{
						PulseForge::AudioSourceComponent Component;
						Component.AudioAsset = Asset.ID;
						RecordComponentOperation(
							Inspector, [Entity, Component] { return Entity.SetAudioSource(Component); }, "Audio source add failed");
						break;
					}
				}
				if (!HasAudioAssets) ImGui::MenuItem("Import an audio asset first", nullptr, false, false);
				ImGui::EndMenu();
			}

			if (Script && !Script->has_value() && ImGui::BeginMenu("Script"))
			{
				bool HasScripts = false;
				for (const PulseForge::AssetRecord& Asset : Inspector.Assets)
				{
					if (!IsAssetExtension(Asset, { ".lua" })) continue;
					HasScripts = true;
					const std::string Path = PathToUtf8(Asset.ProjectRelativePath);
					if (ImGui::MenuItem(Path.c_str()))
					{
						PulseForge::ScriptComponent Component;
						Component.ScriptAsset = Asset.ID;
						RecordComponentOperation(
							Inspector, [Entity, Component] { return Entity.SetScript(Component); }, "Script component add failed");
						break;
					}
				}
				if (!HasScripts) ImGui::MenuItem("Import a Lua script first", nullptr, false, false);
				ImGui::EndMenu();
			}
			ImGui::EndPopup();
		}
	}

	void EditorInspectorPanel::DrawExistingComponents(Context& Inspector, const PulseForge::Entity& Entity)
	{
		DrawCameraComponent(Inspector, Entity);
		DrawDirectionalLightComponent(Inspector, Entity);
		DrawPointLightComponent(Inspector, Entity);
		DrawSpotLightComponent(Inspector, Entity);
		DrawEnvironmentLightComponent(Inspector, Entity);
		DrawMeshRendererComponent(Inspector, Entity);
		DrawRigidbodyComponent(Inspector, Entity);
		DrawBoxColliderComponent(Inspector, Entity);
		DrawAudioSourceComponent(Inspector, Entity);
		DrawAudioListenerComponent(Inspector, Entity);
		DrawScriptComponent(Inspector, Entity);
	}

	void EditorInspectorPanel::DrawEnvironmentLightComponent(Context& Inspector, const PulseForge::Entity& Entity)
	{
		const auto Result = Entity.GetEnvironmentLight();
		if (!Result || !Result->has_value()) return;
		PulseForge::EnvironmentLightComponent Component = **Result;
		const bool Expanded = Inspector.Style.SectionHeader(PulseForgeEditor::EditorIcon::Environment, "Environment Light");
		ImGui::SameLine();
		ImGui::SetCursorPosX((std::max)(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - 31.0f));
		if (Inspector.Style.IconButton(
				PulseForgeEditor::EditorIcon::Delete, "Remove", "RemoveEnvironmentLight", "Remove Environment Light component", true))
		{
			RecordComponentOperation(Inspector, [Entity] { return Entity.RemoveEnvironmentLight(); }, "Environment light removal failed");
			return;
		}
		if (!Expanded) return;
		Inspector.Style.BeginComponentBody("EnvironmentLightComponentBody");
		bool Changed = false;
		std::optional<PulseForge::AssetID> HdrImage{ Component.HdrImage };
		if (Inspector.Style.BeginPropertyTable("EnvironmentLightProperties"))
		{
			if (Inspector.Style.BeginPropertyRow("HDR Image"))
				Changed |= DrawAssetSelector(Inspector, "##EnvironmentHDR", HdrImage, { ".hdr" }, false);
			if (Inspector.Style.BeginPropertyRow("Intensity"))
				Changed |= ImGui::DragFloat("##EnvironmentIntensity", &Component.Intensity, 0.05f, 0.0f, 100000.0f, "%.3f");
			Inspector.Style.EndPropertyTable();
		}
		if (HdrImage) Component.HdrImage = *HdrImage;
		if (Changed)
			RecordComponentOperation(
				Inspector, [Entity, Component] { return Entity.SetEnvironmentLight(Component); }, "Environment light update failed");
		Inspector.Style.EndComponentBody();
	}

	void EditorInspectorPanel::DrawDirectionalLightComponent(Context& Inspector, const PulseForge::Entity& Entity)
	{
		const auto Result = Entity.GetDirectionalLight();
		if (!Result || !Result->has_value()) return;
		PulseForge::DirectionalLightComponent Component = **Result;
		const bool Expanded = Inspector.Style.SectionHeader(PulseForgeEditor::EditorIcon::DirectionalLight, "Directional Light");
		ImGui::SameLine();
		ImGui::SetCursorPosX((std::max)(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - 31.0f));
		if (Inspector.Style.IconButton(
				PulseForgeEditor::EditorIcon::Delete, "Remove", "RemoveDirectionalLight", "Remove Directional Light component", true))
		{
			RecordComponentOperation(Inspector, [Entity] { return Entity.RemoveDirectionalLight(); }, "Directional light removal failed");
			return;
		}
		if (!Expanded) return;
		Inspector.Style.BeginComponentBody("DirectionalLightComponentBody");
		if (Inspector.Style.BeginPropertyTable("DirectionalLightProperties"))
		{
			if (Inspector.Style.BeginPropertyRow("Linear Color"))
				DrawProperty(Inspector, Entity, PulseForge::PropertyComponentIds::DirectionalLight, PulseForge::PropertyIds::Color,
					[](PulseForge::PropertyValue& Value)
					{
						auto* Color = std::get_if<glm::vec3>(&Value);
						return Color && ImGui::DragFloat3("##DirectionalLightLinearColor", &Color->x, 0.01f, 0.0f, 100000.0f, "%.3f");
					});
			if (Inspector.Style.BeginPropertyRow("Intensity"))
				DrawProperty(Inspector, Entity, PulseForge::PropertyComponentIds::DirectionalLight, PulseForge::PropertyIds::Intensity,
					[](PulseForge::PropertyValue& Value)
					{
						auto* Intensity = std::get_if<float>(&Value);
						return Intensity && ImGui::DragFloat("##DirectionalLightIntensity", Intensity, 0.05f, 0.0f, 100000.0f, "%.3f");
					});
			if (Inspector.Style.BeginPropertyRow("Cast Shadows"))
				DrawProperty(Inspector, Entity, PulseForge::PropertyComponentIds::DirectionalLight, PulseForge::PropertyIds::CastShadows,
					[](PulseForge::PropertyValue& Value)
					{
						auto* CastShadows = std::get_if<bool>(&Value);
						return CastShadows && ImGui::Checkbox("##DirectionalLightCastShadows", CastShadows);
					});
			if (Component.CastShadows)
			{
				if (Inspector.Style.BeginPropertyRow("Shadow Distance"))
					DrawProperty(Inspector, Entity, PulseForge::PropertyComponentIds::DirectionalLight,
						PulseForge::PropertyIds::ShadowDistance,
						[](PulseForge::PropertyValue& Value)
						{
							auto* Distance = std::get_if<float>(&Value);
							return Distance &&
								   ImGui::DragFloat("##DirectionalLightShadowDistance", Distance, 0.25f, 0.1f, 5000.0f, "%.1f m");
						});
				if (Inspector.Style.BeginPropertyRow("Shadow Bias"))
					DrawProperty(Inspector, Entity, PulseForge::PropertyComponentIds::DirectionalLight, PulseForge::PropertyIds::ShadowBias,
						[](PulseForge::PropertyValue& Value)
						{
							auto* Bias = std::get_if<float>(&Value);
							return Bias && ImGui::DragFloat("##DirectionalLightShadowBias", Bias, 0.25f, 0.0f, 16.0f, "%.0f units");
						});
				if (Inspector.Style.BeginPropertyRow("Normal Bias"))
					DrawProperty(Inspector, Entity, PulseForge::PropertyComponentIds::DirectionalLight,
						PulseForge::PropertyIds::ShadowNormalBias,
						[](PulseForge::PropertyValue& Value)
						{
							auto* Bias = std::get_if<float>(&Value);
							return Bias && ImGui::DragFloat("##DirectionalLightShadowNormalBias", Bias, 0.0025f, 0.0f, 0.25f, "%.4f");
						});
				if (Inspector.Style.BeginPropertyRow("Softness"))
					DrawProperty(Inspector, Entity, PulseForge::PropertyComponentIds::DirectionalLight,
						PulseForge::PropertyIds::ShadowSoftness,
						[](PulseForge::PropertyValue& Value)
						{
							auto* Softness = std::get_if<float>(&Value);
							return Softness &&
								   ImGui::DragFloat("##DirectionalLightShadowSoftness", Softness, 0.05f, 0.0f, 8.0f, "%.2f texels");
						});
			}
			Inspector.Style.EndPropertyTable();
		}
		Inspector.Style.EndComponentBody();
	}

	void EditorInspectorPanel::DrawPointLightComponent(Context& Inspector, const PulseForge::Entity& Entity)
	{
		const auto Result = Entity.GetPointLight();
		if (!Result || !Result->has_value()) return;
		PulseForge::PointLightComponent Component = **Result;
		const bool Expanded = Inspector.Style.SectionHeader(PulseForgeEditor::EditorIcon::PointLight, "Point Light");
		ImGui::SameLine();
		ImGui::SetCursorPosX((std::max)(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - 31.0f));
		if (Inspector.Style.IconButton(
				PulseForgeEditor::EditorIcon::Delete, "Remove", "RemovePointLight", "Remove Point Light component", true))
		{
			RecordComponentOperation(Inspector, [Entity] { return Entity.RemovePointLight(); }, "Point light removal failed");
			return;
		}
		if (!Expanded) return;
		Inspector.Style.BeginComponentBody("PointLightComponentBody");
		if (Inspector.Style.BeginPropertyTable("PointLightProperties"))
		{
			if (Inspector.Style.BeginPropertyRow("Linear Color"))
				DrawProperty(Inspector, Entity, PulseForge::PropertyComponentIds::PointLight, PulseForge::PropertyIds::Color,
					[](PulseForge::PropertyValue& Value)
					{
						auto* Color = std::get_if<glm::vec3>(&Value);
						return Color && ImGui::DragFloat3("##PointLightColor", &Color->x, 0.01f, 0.0f, 1000.0f, "%.3f");
					});
			if (Inspector.Style.BeginPropertyRow("Intensity (unitless)"))
				DrawProperty(Inspector, Entity, PulseForge::PropertyComponentIds::PointLight, PulseForge::PropertyIds::Intensity,
					[](PulseForge::PropertyValue& Value)
					{
						auto* Intensity = std::get_if<float>(&Value);
						return Intensity && ImGui::DragFloat("##PointLightIntensity", Intensity, 0.1f, 0.0f, 1000000.0f, "%.2f");
					});
			if (Inspector.Style.BeginPropertyRow("Range"))
				DrawProperty(Inspector, Entity, PulseForge::PropertyComponentIds::PointLight, PulseForge::PropertyIds::Range,
					[](PulseForge::PropertyValue& Value)
					{
						auto* Range = std::get_if<float>(&Value);
						return Range && ImGui::DragFloat("##PointLightRange", Range, 0.05f, 0.01f, 10000.0f, "%.2f units");
					});
			if (Inspector.Style.BeginPropertyRow("Cast Shadows"))
				DrawProperty(Inspector, Entity, PulseForge::PropertyComponentIds::PointLight, PulseForge::PropertyIds::CastShadows,
					[](PulseForge::PropertyValue& Value)
					{
						auto* CastShadows = std::get_if<bool>(&Value);
						return CastShadows && ImGui::Checkbox("##PointLightCastShadows", CastShadows);
					});
			if (Component.CastShadows)
			{
				if (Inspector.Style.BeginPropertyRow("Shadow Bias (normalized)"))
					DrawProperty(Inspector, Entity, PulseForge::PropertyComponentIds::PointLight, PulseForge::PropertyIds::ShadowBias,
						[](PulseForge::PropertyValue& Value)
						{
							auto* Bias = std::get_if<float>(&Value);
							return Bias && ImGui::DragFloat(
											   "##PointLightShadowBias", Bias, 0.0001f, 0.0f, PulseForge::MaxPointLightShadowBias, "%.4f");
						});
				if (Inspector.Style.BeginPropertyRow("Shadow Normal Bias"))
					DrawProperty(Inspector, Entity, PulseForge::PropertyComponentIds::PointLight, PulseForge::PropertyIds::ShadowNormalBias,
						[](PulseForge::PropertyValue& Value)
						{
							auto* Bias = std::get_if<float>(&Value);
							return Bias && ImGui::DragFloat("##PointLightShadowNormalBias", Bias, 0.001f, 0.0f,
											   PulseForge::MaxPointLightShadowNormalBias, "%.3f units");
						});
				if (Inspector.Style.BeginPropertyRow("Shadow Softness (texels)"))
					DrawProperty(Inspector, Entity, PulseForge::PropertyComponentIds::PointLight, PulseForge::PropertyIds::ShadowSoftness,
						[](PulseForge::PropertyValue& Value)
						{
							auto* Softness = std::get_if<float>(&Value);
							return Softness && ImGui::DragFloat("##PointLightShadowSoftness", Softness, 0.05f, 0.0f,
												   PulseForge::MaxPointLightShadowSoftness, "%.2f");
						});
			}
			Inspector.Style.EndPropertyTable();
		}
		Inspector.Style.EndComponentBody();
	}

	void EditorInspectorPanel::DrawSpotLightComponent(Context& Inspector, const PulseForge::Entity& Entity)
	{
		const auto Result = Entity.GetSpotLight();
		if (!Result || !Result->has_value()) return;
		PulseForge::SpotLightComponent Component = **Result;
		const bool Expanded = Inspector.Style.SectionHeader(PulseForgeEditor::EditorIcon::SpotLight, "Spot Light");
		ImGui::SameLine();
		ImGui::SetCursorPosX((std::max)(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - 31.0f));
		if (Inspector.Style.IconButton(
				PulseForgeEditor::EditorIcon::Delete, "Remove", "RemoveSpotLight", "Remove Spot Light component", true))
		{
			RecordComponentOperation(Inspector, [Entity] { return Entity.RemoveSpotLight(); }, "Spot light removal failed");
			return;
		}
		if (!Expanded) return;
		Inspector.Style.BeginComponentBody("SpotLightComponentBody");
		if (Inspector.Style.BeginPropertyTable("SpotLightProperties"))
		{
			if (Inspector.Style.BeginPropertyRow("Linear Color"))
				DrawProperty(Inspector, Entity, PulseForge::PropertyComponentIds::SpotLight, PulseForge::PropertyIds::Color,
					[](PulseForge::PropertyValue& Value)
					{
						auto* Color = std::get_if<glm::vec3>(&Value);
						return Color && ImGui::DragFloat3("##SpotLightColor", &Color->x, 0.01f, 0.0f, 1000.0f, "%.3f");
					});
			if (Inspector.Style.BeginPropertyRow("Intensity (unitless)"))
				DrawProperty(Inspector, Entity, PulseForge::PropertyComponentIds::SpotLight, PulseForge::PropertyIds::Intensity,
					[](PulseForge::PropertyValue& Value)
					{
						auto* Intensity = std::get_if<float>(&Value);
						return Intensity && ImGui::DragFloat("##SpotLightIntensity", Intensity, 0.1f, 0.0f, 1000000.0f, "%.2f");
					});
			if (Inspector.Style.BeginPropertyRow("Range"))
				DrawProperty(Inspector, Entity, PulseForge::PropertyComponentIds::SpotLight, PulseForge::PropertyIds::Range,
					[](PulseForge::PropertyValue& Value)
					{
						auto* Range = std::get_if<float>(&Value);
						return Range && ImGui::DragFloat("##SpotLightRange", Range, 0.05f, 0.01f, 10000.0f, "%.2f units");
					});
			if (Inspector.Style.BeginPropertyRow("Inner cone half-angle"))
				DrawProperty(Inspector, Entity, PulseForge::PropertyComponentIds::SpotLight, PulseForge::PropertyIds::InnerConeAngleDegrees,
					[&Component](PulseForge::PropertyValue& Value)
					{
						auto* Angle = std::get_if<float>(&Value);
						return Angle &&
							   ImGui::DragFloat("##SpotLightInner", Angle, 0.25f, 0.5f, Component.OuterConeAngleDegrees - 0.1f, "%.1f deg");
					});
			if (Inspector.Style.BeginPropertyRow("Outer cone half-angle"))
				DrawProperty(Inspector, Entity, PulseForge::PropertyComponentIds::SpotLight, PulseForge::PropertyIds::OuterConeAngleDegrees,
					[&Component](PulseForge::PropertyValue& Value)
					{
						auto* Angle = std::get_if<float>(&Value);
						return Angle && ImGui::DragFloat(
											"##SpotLightOuter", Angle, 0.25f, Component.InnerConeAngleDegrees + 0.1f, 89.5f, "%.1f deg");
					});
			if (Inspector.Style.BeginPropertyRow("Cast Shadows"))
				DrawProperty(Inspector, Entity, PulseForge::PropertyComponentIds::SpotLight, PulseForge::PropertyIds::CastShadows,
					[](PulseForge::PropertyValue& Value)
					{
						auto* CastShadows = std::get_if<bool>(&Value);
						return CastShadows && ImGui::Checkbox("##SpotLightCastShadows", CastShadows);
					});
			if (Inspector.Style.BeginPropertyRow("Shadow Bias"))
				DrawProperty(Inspector, Entity, PulseForge::PropertyComponentIds::SpotLight, PulseForge::PropertyIds::ShadowBias,
					[](PulseForge::PropertyValue& Value)
					{
						auto* Bias = std::get_if<float>(&Value);
						return Bias && ImGui::DragFloat("##SpotLightShadowBias", Bias, 0.0001f, 0.0f, 0.02f, "%.4f");
					});
			if (Inspector.Style.BeginPropertyRow("Shadow Normal Bias"))
				DrawProperty(Inspector, Entity, PulseForge::PropertyComponentIds::SpotLight, PulseForge::PropertyIds::ShadowNormalBias,
					[](PulseForge::PropertyValue& Value)
					{
						auto* Bias = std::get_if<float>(&Value);
						return Bias && ImGui::DragFloat("##SpotLightShadowNormalBias", Bias, 0.0025f, 0.0f, 1.0f, "%.3f units");
					});
			if (Inspector.Style.BeginPropertyRow("Shadow Softness"))
				DrawProperty(Inspector, Entity, PulseForge::PropertyComponentIds::SpotLight, PulseForge::PropertyIds::ShadowSoftness,
					[](PulseForge::PropertyValue& Value)
					{
						auto* Softness = std::get_if<float>(&Value);
						return Softness && ImGui::DragFloat("##SpotLightShadowSoftness", Softness, 0.1f, 0.0f, 4.0f, "%.1f texels");
					});
			if (Component.CastShadows && Component.OuterConeAngleDegrees > 80.0f)
				ImGui::TextDisabled("Shadows support outer half-angles up to 80 degrees; illumination is unchanged.");
			Inspector.Style.EndPropertyTable();
		}
		Inspector.Style.EndComponentBody();
	}

	void EditorInspectorPanel::DrawCameraComponent(Context& Inspector, const PulseForge::Entity& Entity)
	{
		const auto Result = Entity.GetCamera();
		if (!Result || !Result->has_value()) return;

		PulseForge::CameraComponent Component = **Result;
		const bool IsExpanded = Inspector.Style.SectionHeader(PulseForgeEditor::EditorIcon::Camera, "Camera");
		ImGui::SameLine();
		ImGui::SetCursorPosX((std::max)(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - 31.0f));
		if (Inspector.Style.IconButton(PulseForgeEditor::EditorIcon::Delete, "Remove", "RemoveCamera", "Remove Camera component", true))
		{
			RecordComponentOperation(Inspector, [Entity] { return Entity.RemoveCamera(); }, "Camera component removal failed");
			return;
		}
		if (!IsExpanded) return;
		Inspector.Style.BeginComponentBody("CameraComponentBody");

		float FieldOfViewDegrees = glm::degrees(Component.VerticalFieldOfViewRadians);
		const float MaxNearClip = (std::max)(0.002f, Component.FarClipPlane - 0.001f);
		const float MinFarClip = Component.NearClipPlane + 0.001f;
		bool FieldOfViewChanged = false;
		bool NearChanged = false;
		bool FarChanged = false;
		bool PrimaryChanged = false;
		if (Inspector.Style.BeginPropertyTable("CameraProperties"))
		{
			if (Inspector.Style.BeginPropertyRow("Vertical FOV"))
				FieldOfViewChanged = ImGui::DragFloat("##CameraFov", &FieldOfViewDegrees, 0.1f, 1.0f, 179.0f, "%.1f deg");
			if (Inspector.Style.BeginPropertyRow("Near Clip"))
				NearChanged = ImGui::DragFloat("##CameraNear", &Component.NearClipPlane, 0.01f, 0.001f, MaxNearClip, "%.3f");
			if (Inspector.Style.BeginPropertyRow("Far Clip"))
				FarChanged = ImGui::DragFloat("##CameraFar", &Component.FarClipPlane, 1.0f, MinFarClip, 1000000.0f, "%.1f");
			if (Inspector.Style.BeginPropertyRow("Primary")) PrimaryChanged = ImGui::Checkbox("##PrimaryCamera", &Component.IsPrimary);
			Inspector.Style.EndPropertyTable();
		}
		if (FieldOfViewChanged || NearChanged || FarChanged || PrimaryChanged)
		{
			Component.VerticalFieldOfViewRadians = glm::radians(FieldOfViewDegrees);
			SetCameraComponent(Inspector, Entity, Component);
		}
		Inspector.Style.EndComponentBody();
	}

	void EditorInspectorPanel::DrawMeshRendererComponent(Context& Inspector, const PulseForge::Entity& Entity)
	{
		const auto Result = Entity.GetMeshRenderer();
		if (!Result || !Result->has_value()) return;

		const bool IsExpanded = Inspector.Style.SectionHeader(PulseForgeEditor::EditorIcon::Cube, "Mesh Renderer");
		ImGui::SameLine();
		ImGui::SetCursorPosX((std::max)(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - 31.0f));
		if (Inspector.Style.IconButton(
				PulseForgeEditor::EditorIcon::Delete, "Remove", "RemoveMeshRenderer", "Remove Mesh Renderer component", true))
		{
			RecordComponentOperation(Inspector, [Entity] { return Entity.RemoveMeshRenderer(); }, "Mesh renderer removal failed");
			return;
		}
		if (!IsExpanded) return;
		Inspector.Style.BeginComponentBody("MeshRendererComponentBody");

		if (Inspector.Style.BeginPropertyTable("MeshRendererProperties"))
		{
			if (Inspector.Style.BeginPropertyRow("Mesh Asset"))
				DrawProperty(Inspector, Entity, PulseForge::PropertyComponentIds::MeshRenderer, PulseForge::PropertyIds::MeshAsset,
					[&Inspector, this](PulseForge::PropertyValue& Value)
					{
						auto* Mesh = std::get_if<PulseForge::AssetID>(&Value);
						if (!Mesh) return false;
						std::optional<PulseForge::AssetID> Selected{ *Mesh };
						if (!DrawAssetSelector(Inspector, "##MeshAsset", Selected, { ".gltf", ".glb" }, false) || !Selected) return false;
						*Mesh = *Selected;
						return true;
					});
			if (Inspector.Style.BeginPropertyRow("Material"))
				DrawProperty(Inspector, Entity, PulseForge::PropertyComponentIds::MeshRenderer, PulseForge::PropertyIds::MaterialAsset,
					[&Inspector, this](PulseForge::PropertyValue& Value)
					{
						auto* Material = std::get_if<std::optional<PulseForge::AssetID>>(&Value);
						return Material && DrawAssetSelector(Inspector, "##MaterialAsset", *Material, { ".material" }, true);
					});
			Inspector.Style.EndPropertyTable();
		}
		Inspector.Style.EndComponentBody();
	}

	void EditorInspectorPanel::DrawRigidbodyComponent(Context& Inspector, const PulseForge::Entity& Entity)
	{
		const auto Result = Entity.GetRigidbody();
		if (!Result || !Result->has_value()) return;

		PulseForge::RigidbodyComponent Component = **Result;
		const bool IsExpanded = Inspector.Style.SectionHeader(PulseForgeEditor::EditorIcon::Settings, "Rigidbody");
		ImGui::SameLine();
		ImGui::SetCursorPosX((std::max)(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - 31.0f));
		if (Inspector.Style.IconButton(
				PulseForgeEditor::EditorIcon::Delete, "Remove", "RemoveRigidbody", "Remove Rigidbody component", true))
		{
			RecordComponentOperation(Inspector, [Entity] { return Entity.RemoveRigidbody(); }, "Rigidbody removal failed");
			return;
		}
		if (!IsExpanded) return;
		Inspector.Style.BeginComponentBody("RigidbodyComponentBody");

		int MotionType = Component.MotionType == PulseForge::RigidbodyMotionType::Static ? 0 : 1;
		const char* MotionTypes[]{ "Static", "Dynamic" };
		bool Changed = false;
		if (Inspector.Style.BeginPropertyTable("RigidbodyProperties"))
		{
			if (Inspector.Style.BeginPropertyRow("Motion Type") && ImGui::Combo("##RigidbodyMotionType", &MotionType, MotionTypes, 2))
			{
				Component.MotionType = MotionType == 0 ? PulseForge::RigidbodyMotionType::Static : PulseForge::RigidbodyMotionType::Dynamic;
				Changed = true;
			}
			if (Inspector.Style.BeginPropertyRow("Mass"))
				Changed |= ImGui::DragFloat("##RigidbodyMass", &Component.Mass, 0.05f, 0.001f, 1000000.0f, "%.2f");
			if (Inspector.Style.BeginPropertyRow("Friction"))
				Changed |= ImGui::DragFloat("##RigidbodyFriction", &Component.Friction, 0.01f, 0.0f, 1.0f, "%.2f");
			if (Inspector.Style.BeginPropertyRow("Restitution"))
				Changed |= ImGui::DragFloat("##RigidbodyRestitution", &Component.Restitution, 0.01f, 0.0f, 1.0f, "%.2f");
			if (Inspector.Style.BeginPropertyRow("Allow Sleeping"))
				Changed |= ImGui::Checkbox("##RigidbodySleeping", &Component.AllowSleeping);
			Inspector.Style.EndPropertyTable();
		}
		if (Changed)
			RecordComponentOperation(Inspector, [Entity, Component] { return Entity.SetRigidbody(Component); }, "Rigidbody update failed");
		Inspector.Style.EndComponentBody();
	}

	void EditorInspectorPanel::DrawBoxColliderComponent(Context& Inspector, const PulseForge::Entity& Entity)
	{
		const auto Result = Entity.GetBoxCollider();
		if (!Result || !Result->has_value()) return;

		PulseForge::BoxColliderComponent Component = **Result;
		const bool IsExpanded = Inspector.Style.SectionHeader(PulseForgeEditor::EditorIcon::Cube, "Box Collider");
		ImGui::SameLine();
		ImGui::SetCursorPosX((std::max)(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - 31.0f));
		if (Inspector.Style.IconButton(
				PulseForgeEditor::EditorIcon::Delete, "Remove", "RemoveBoxCollider", "Remove Box Collider component", true))
		{
			RecordComponentOperation(Inspector, [Entity] { return Entity.RemoveBoxCollider(); }, "Box collider removal failed");
			return;
		}
		if (!IsExpanded) return;
		Inspector.Style.BeginComponentBody("BoxColliderComponentBody");

		float HalfExtents[3]{ Component.HalfExtents.x, Component.HalfExtents.y, Component.HalfExtents.z };
		bool Changed = false;
		if (Inspector.Style.BeginPropertyTable("BoxColliderProperties"))
		{
			if (Inspector.Style.BeginPropertyRow("Half Extents"))
				Changed = ImGui::DragFloat3("##BoxColliderExtents", HalfExtents, 0.01f, 0.001f, 1000000.0f, "%.2f");
			Inspector.Style.EndPropertyTable();
		}
		if (Changed)
		{
			Component.HalfExtents = { HalfExtents[0], HalfExtents[1], HalfExtents[2] };
			RecordComponentOperation(
				Inspector, [Entity, Component] { return Entity.SetBoxCollider(Component); }, "Box collider update failed");
		}
		Inspector.Style.EndComponentBody();
	}

	void EditorInspectorPanel::DrawAudioSourceComponent(Context& Inspector, const PulseForge::Entity& Entity)
	{
		const auto Result = Entity.GetAudioSource();
		if (!Result || !Result->has_value()) return;

		PulseForge::AudioSourceComponent Component = **Result;
		const bool IsExpanded = Inspector.Style.SectionHeader(PulseForgeEditor::EditorIcon::AudioSource, "Audio Source");
		ImGui::SameLine();
		ImGui::SetCursorPosX((std::max)(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - 31.0f));
		if (Inspector.Style.IconButton(
				PulseForgeEditor::EditorIcon::Delete, "Remove", "RemoveAudioSource", "Remove Audio Source component", true))
		{
			RecordComponentOperation(Inspector, [Entity] { return Entity.RemoveAudioSource(); }, "Audio source removal failed");
			return;
		}
		if (!IsExpanded) return;
		Inspector.Style.BeginComponentBody("AudioSourceComponentBody");

		std::optional<PulseForge::AssetID> Audio = Component.AudioAsset;
		bool Changed = false;
		if (Inspector.Style.BeginPropertyTable("AudioSourceProperties"))
		{
			if (Inspector.Style.BeginPropertyRow("Audio Asset"))
				Changed = DrawAssetSelector(Inspector, "##AudioAsset", Audio, { ".wav", ".mp3", ".flac", ".ogg" }, false);
			if (Inspector.Style.BeginPropertyRow("Volume"))
				Changed |= ImGui::DragFloat("##AudioVolume", &Component.Volume, 0.01f, 0.0f, 1.0f, "%.2f");
			if (Inspector.Style.BeginPropertyRow("Looping")) Changed |= ImGui::Checkbox("##AudioLooping", &Component.Looping);
			if (Inspector.Style.BeginPropertyRow("Play On Start")) Changed |= ImGui::Checkbox("##AudioPlayOnStart", &Component.PlayOnStart);
			if (Inspector.Style.BeginPropertyRow("Spatialized")) Changed |= ImGui::Checkbox("##AudioSpatialized", &Component.Spatialized);
			Inspector.Style.EndPropertyTable();
		}
		if (Changed && Audio)
		{
			Component.AudioAsset = *Audio;
			RecordComponentOperation(
				Inspector, [Entity, Component] { return Entity.SetAudioSource(Component); }, "Audio source update failed");
		}
		Inspector.Style.EndComponentBody();
	}

	void EditorInspectorPanel::DrawAudioListenerComponent(Context& Inspector, const PulseForge::Entity& Entity)
	{
		const auto Result = Entity.GetAudioListener();
		if (!Result || !Result->has_value()) return;

		PulseForge::AudioListenerComponent Component = **Result;
		const bool IsExpanded = Inspector.Style.SectionHeader(PulseForgeEditor::EditorIcon::AudioListener, "Audio Listener");
		ImGui::SameLine();
		ImGui::SetCursorPosX((std::max)(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - 31.0f));
		if (Inspector.Style.IconButton(
				PulseForgeEditor::EditorIcon::Delete, "Remove", "RemoveAudioListener", "Remove Audio Listener component", true))
		{
			RecordComponentOperation(Inspector, [Entity] { return Entity.RemoveAudioListener(); }, "Audio listener removal failed");
			return;
		}
		if (!IsExpanded) return;
		Inspector.Style.BeginComponentBody("AudioListenerComponentBody");
		if (Inspector.Style.BeginPropertyTable("AudioListenerProperties"))
		{
			if (Inspector.Style.BeginPropertyRow("Primary Listener") && ImGui::Checkbox("##PrimaryAudioListener", &Component.IsPrimary))
				SetAudioListenerComponent(Inspector, Entity, Component);
			Inspector.Style.EndPropertyTable();
		}
		Inspector.Style.EndComponentBody();
	}

	void EditorInspectorPanel::DrawScriptComponent(Context& Inspector, const PulseForge::Entity& Entity)
	{
		const auto Result = Entity.GetScript();
		if (!Result || !Result->has_value()) return;

		const bool IsExpanded = Inspector.Style.SectionHeader(PulseForgeEditor::EditorIcon::Script, "Script");
		ImGui::SameLine();
		ImGui::SetCursorPosX((std::max)(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - 31.0f));
		if (Inspector.Style.IconButton(PulseForgeEditor::EditorIcon::Delete, "Remove", "RemoveScript", "Remove Script component", true))
		{
			RecordComponentOperation(Inspector, [Entity] { return Entity.RemoveScript(); }, "Script component removal failed");
			return;
		}
		if (!IsExpanded) return;
		Inspector.Style.BeginComponentBody("ScriptComponentBody");

		if (Inspector.Style.BeginPropertyTable("ScriptProperties"))
		{
			if (Inspector.Style.BeginPropertyRow("Lua Script"))
				DrawProperty(Inspector, Entity, PulseForge::PropertyComponentIds::Script, PulseForge::PropertyIds::Asset,
					[&Inspector, this](PulseForge::PropertyValue& Value)
					{
						auto* ScriptAsset = std::get_if<PulseForge::AssetID>(&Value);
						if (!ScriptAsset) return false;
						std::optional<PulseForge::AssetID> Script{ *ScriptAsset };
						if (!DrawAssetSelector(Inspector, "##ScriptAsset", Script, { ".lua" }, false) || !Script) return false;
						*ScriptAsset = *Script;
						return true;
					});
			if (Inspector.Style.BeginPropertyRow("Enabled"))
				DrawProperty(Inspector, Entity, PulseForge::PropertyComponentIds::Script, PulseForge::PropertyIds::Enabled,
					[](PulseForge::PropertyValue& Value)
					{
						auto* Enabled = std::get_if<bool>(&Value);
						return Enabled && ImGui::Checkbox("##ScriptEnabled", Enabled);
					});
			Inspector.Style.EndPropertyTable();
		}
		DrawScriptFloatProperties(Inspector, Entity);
		Inspector.Style.EndComponentBody();
	}

	void EditorInspectorPanel::DrawScriptFloatProperties(Context& Inspector, const PulseForge::Entity& Entity)
	{
		const auto UpdateScript = [this, &Inspector](
									  std::string Description, std::function<std::expected<void, PulseForge::SceneError>()> Update)
		{ RecordComponentOperation(Inspector, std::move(Update), Description); };

		ImGui::TextDisabled("Script Properties");
		const auto Properties = PulseForge::GetDynamicPropertyDescriptors(Entity, PulseForge::PropertyComponentIds::ScriptFloat);
		if (!Properties)
		{
			Inspector.Actions.ReportInspectorError("Could not inspect script properties: " + Properties.error().Message);
			return;
		}
		if (!Properties->empty() && Inspector.Style.BeginPropertyTable("ScriptFloatProperties"))
		{
			for (const PulseForge::DynamicPropertyDescriptor& Property : *Properties)
			{
				if (!Property.Editable) continue;
				ImGui::PushID(Property.Id.c_str());
				if (Inspector.Style.BeginPropertyRow(Property.Label))
				{
					const float AvailableWidth = (std::max)(ImGui::GetContentRegionAvail().x, 1.0f);
					const float RemoveWidth = (std::min)(31.0f, AvailableWidth * 0.35f);
					ImGui::SetNextItemWidth((std::max)(AvailableWidth - RemoveWidth - ImGui::GetStyle().ItemInnerSpacing.x, 1.0f));
					DrawProperty(Inspector, Entity, Property.ComponentId, Property.Id,
						[](PulseForge::PropertyValue& Value)
						{
							auto* Number = std::get_if<float>(&Value);
							return Number && ImGui::DragFloat("##ScriptFloatProperty", Number, 0.05f,
												 -PulseForge::MaximumScriptFloatPropertyMagnitude,
												 PulseForge::MaximumScriptFloatPropertyMagnitude, "%.3f");
						});
					ImGui::SameLine();
					if (ImGui::SmallButton("X"))
						UpdateScript("Remove script property " + Property.Id,
							[Entity, Identifier = Property.Id]() -> std::expected<void, PulseForge::SceneError>
							{
								const auto Removed = PulseForge::RemoveScriptFloatProperty(Entity, Identifier);
								if (!Removed) return std::unexpected(ToSceneError(Removed.error()));
								return {};
							});
				}
				ImGui::PopID();
			}
			Inspector.Style.EndPropertyTable();
		}

		const float AddRowWidth = (std::max)(ImGui::GetContentRegionAvail().x, 1.0f);
		ImGui::SetNextItemWidth((std::max)(AddRowWidth * 0.45f, 1.0f));
		ImGui::InputText("##NewScriptPropertyIdentifier", &m_NewScriptPropertyIdentifier);
		ImGui::SameLine();
		ImGui::SetNextItemWidth((std::max)(AddRowWidth * 0.30f, 1.0f));
		ImGui::DragFloat("##NewScriptPropertyValue", &m_NewScriptPropertyValue, 0.05f, -PulseForge::MaximumScriptFloatPropertyMagnitude,
			PulseForge::MaximumScriptFloatPropertyMagnitude, "%.3f");
		ImGui::SameLine();
		if (ImGui::SmallButton("Add"))
		{
			if (!PulseForge::IsValidScriptPropertyIdentifier(m_NewScriptPropertyIdentifier))
				Inspector.Actions.ReportInspectorError("Script property IDs must use the supported ASCII identifier format.");
			else if (std::any_of(Properties->begin(), Properties->end(), [this](const PulseForge::DynamicPropertyDescriptor& Property)
						 { return Property.Id == m_NewScriptPropertyIdentifier; }))
				Inspector.Actions.ReportInspectorError("A script property with that ID already exists.");
			else
				UpdateScript("Add script property " + m_NewScriptPropertyIdentifier,
					[Entity, Identifier = m_NewScriptPropertyIdentifier,
						Value = m_NewScriptPropertyValue]() -> std::expected<void, PulseForge::SceneError>
					{
						const auto Added = PulseForge::AddScriptFloatProperty(Entity, Identifier, Value);
						if (!Added) return std::unexpected(ToSceneError(Added.error()));
						return {};
					});
		}
	}

} // namespace PulseForgeEditor
