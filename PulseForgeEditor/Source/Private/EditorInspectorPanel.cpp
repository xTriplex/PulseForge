#include "Editor/EditorInspectorPanel.h"

#include "Editor/EditorAssetSelector.h"
#include "Editor/EditorAssetSelectorPolicy.h"
#include "Editor/EditorHierarchyPanel.h"
#include "Editor/EditorHierarchyPanelPolicy.h"
#include "Editor/EditorSceneWorkspaceState.h"
#include "Editor/EditorStyle.h"
#include "Assets/PrefabInstanceReconciler.h"

#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <expected>
#include <functional>
#include <initializer_list>
#include <iterator>

namespace PulseForgeEditor
{
	void DispatchScriptDiagnosticOpen(EditorInspectorPanelActions& Actions, const PulseForge::ScriptDiagnostic& Diagnostic)
	{
		auto Opened = Actions.OpenScriptDiagnostic(Diagnostic);
		if (!Opened) Actions.ReportInspectorError("Could not open diagnostic script source: " + Opened.error());
	}

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

		const char* ScriptDiagnosticName(PulseForge::ScriptDiagnosticCode Code) noexcept
		{
			using PulseForge::ScriptDiagnosticCode;
			switch (Code)
			{
			case ScriptDiagnosticCode::InvalidComponent: return "Invalid component";
			case ScriptDiagnosticCode::AssetNotFound: return "Asset not found";
			case ScriptDiagnosticCode::WrongAssetType: return "Wrong asset type";
			case ScriptDiagnosticCode::FileReadFailed: return "File read failed";
			case ScriptDiagnosticCode::LuaInitializationFailed: return "Lua initialization failed";
			case ScriptDiagnosticCode::CompileFailed: return "Compile failed";
			case ScriptDiagnosticCode::InitializationFailed: return "Initialization failed";
			case ScriptDiagnosticCode::UpdateFailed: return "Update failed";
			case ScriptDiagnosticCode::DestroyFailed: return "Destroy failed";
			case ScriptDiagnosticCode::ReloadRejected: return "Reload rejected";
			case ScriptDiagnosticCode::ReloadActivationFailed: return "Reload activation failed";
			case ScriptDiagnosticCode::ModuleLoadFailed: return "Module load failed";
			}
			return "Script diagnostic";
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
						const auto Updated = Actions.RunInspectorSceneEdit("Rename Entity",
							[Entity = *Entity, Tag = *Tag, Actions = &Actions]() -> std::expected<void, PulseForge::SceneError>
							{
								if (auto Result = Entity.SetTag(Tag); !Result) return Result;
								if (auto Result = Actions->RecordPrefabRenameOverride(Entity, Tag.Name); !Result)
									return std::unexpected(PulseForge::SceneError{
										PulseForge::SceneErrorCode::InvalidPrefabInstance, Result.error() });
								return {};
							});
						if (!Updated) Actions.ReportInspectorError("Entity name update failed: " + Updated.error());
					}
					ImGui::SetCursorPosX(ImGui::GetStyle().WindowPadding.x + 29.0f);
					Style.TextMuted(Entity->GetUUID().ToString());
				}
				Style.EndToolbar();
				if (Actions.IsLinkedPrefabEntity(*Entity))
				{
					ImGui::TextDisabled("Linked prefab instance. Typed property edits are recorded as overrides.");
					ImGui::TextDisabled("Component add/remove and untyped component edits are not supported on this instance.");
				}
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
		if (Inspector.SceneValue)
			if (const auto Selected = Inspector.Workspace.Selection().GetPrimary())
				if (const auto Entity = Inspector.SceneValue->FindEntity(*Selected);
					Entity && Inspector.Actions.IsLinkedPrefabEntity(*Entity))
				{
					Inspector.Actions.ReportInspectorError(
						"This component operation is not yet supported as a linked-instance override. "
						"Edit the prefab source or use a supported typed property.");
					return;
				}
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
				Value = std::move(*Value), Actions = &Inspector.Actions]() -> std::expected<void, PulseForge::SceneError>
			{
				const auto Result = PulseForge::WriteProperty(Entity, ComponentId, PropertyId, Value);
				if (!Result)
					return std::unexpected(PulseForge::SceneError{ PulseForge::SceneErrorCode::StorageFailure, Result.error().Message });
				if (auto Recorded = Actions->RecordPrefabPropertyOverride(Entity, ComponentId, PropertyId); !Recorded)
					return std::unexpected(PulseForge::SceneError{
						PulseForge::SceneErrorCode::InvalidPrefabInstance, Recorded.error() });
				return {};
			});
		if (!Updated) Inspector.Actions.ReportInspectorError("Inspector property update failed: " + Updated.error());

		if (!Inspector.SceneValue) return;
		auto LinkedRoot = PulseForge::PrefabInstanceReconciler::FindLinkedRoot(*Inspector.SceneValue, Entity);
		if (!LinkedRoot || !LinkedRoot->has_value()) return;
		const auto Metadata = (**LinkedRoot).GetPrefabInstance();
		if (!Metadata || !Metadata->has_value()) return;
		std::optional<PulseForge::PrefabEntityAddress> Address;
		for (const PulseForge::PrefabEntityMapping& Mapping : (**Metadata).SourceEntities)
			if (Mapping.SceneEntity == Entity.GetUUID()) Address = PulseForge::PrefabEntityAddress{ Mapping.SourceKey, false };
		if (!Address) return; // Local entities have no source value to reset to.
		const bool HasOverride = std::any_of((**Metadata).Overrides.begin(), (**Metadata).Overrides.end(),
			[&](const PulseForge::PrefabOverrideRecord& Override)
			{
				return Override.Entity == *Address && Override.Operation == PulseForge::PrefabOverrideOperation::SetProperty &&
					Override.ComponentID == ComponentId && Override.PropertyID == PropertyId;
			});
		if (!HasOverride) return;
		ImGui::SameLine();
		const std::string ResetLabel = "Reset override##" + std::string(ComponentId) + std::string(PropertyId) + Entity.GetUUID().ToString();
		if (!ImGui::SmallButton(ResetLabel.c_str())) return;
		auto Reset = Inspector.Actions.ResetPrefabPropertyOverride(Entity, ComponentId, PropertyId);
		if (!Reset) Inspector.Actions.ReportInspectorError("Prefab override reset failed: " + Reset.error());
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
			const auto RuntimeUICanvas = Entity.GetRuntimeUICanvas();
			if (!Camera || !DirectionalLight || !EnvironmentLight || !PointLight || !SpotLight || !Mesh || !Rigidbody || !Collider ||
				!AudioSource || !AudioListener || !Script || !RuntimeUICanvas)
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
			if (Collider && !Collider->has_value() && ImGui::MenuItem("Collider"))
				RecordComponentOperation(
					Inspector, [Entity] { return Entity.SetCollider(PulseForge::BoxColliderComponent{}); }, "Collider add failed");
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

			if (RuntimeUICanvas && !RuntimeUICanvas->has_value() && ImGui::BeginMenu("Runtime UI Canvas"))
			{
				bool HasDocuments = false;
				for (const PulseForge::AssetRecord& Asset : Inspector.Assets)
				{
					if (!IsAssetExtension(Asset, { ".pfui" })) continue;
					HasDocuments = true;
					const std::string Path = PathToUtf8(Asset.ProjectRelativePath);
					if (ImGui::MenuItem(Path.c_str()))
					{
						const PulseForge::RuntimeUICanvasComponent Component{ Asset.ID, true };
						RecordComponentOperation(Inspector, [Entity, Component] { return Entity.SetRuntimeUICanvas(Component); },
							"Runtime UI canvas assignment failed");
						break;
					}
				}
				if (!HasDocuments) ImGui::MenuItem("Create a .pfui asset first", nullptr, false, false);
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
		DrawRuntimeUICanvasComponent(Inspector, Entity);
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
		const bool IsExpanded = Inspector.Style.SectionHeader(PulseForgeEditor::EditorIcon::Cube, "Collider");
		ImGui::SameLine();
		ImGui::SetCursorPosX((std::max)(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - 31.0f));
		if (Inspector.Style.IconButton(
				PulseForgeEditor::EditorIcon::Delete, "Remove", "RemoveCollider", "Remove Collider component", true))
		{
			RecordComponentOperation(Inspector, [Entity] { return Entity.RemoveCollider(); }, "Collider removal failed");
			return;
		}
		if (!IsExpanded) return;
		Inspector.Style.BeginComponentBody("BoxColliderComponentBody");

		if (Inspector.Style.BeginPropertyTable("BoxColliderProperties"))
		{
			if (Inspector.Style.BeginPropertyRow("Shape"))
				DrawProperty(Inspector, Entity, PulseForge::PropertyComponentIds::Collider, PulseForge::PropertyIds::Shape,
					[](PulseForge::PropertyValue& Value)
					{
						auto& Shape = std::get<std::string>(Value);
						constexpr const char* Shapes[]{ "box", "sphere", "capsule" };
						int Selected = Shape == "sphere" ? 1 : Shape == "capsule" ? 2 : 0;
						if (!ImGui::Combo("##ColliderShape", &Selected, Shapes, 3)) return false;
						Shape = Shapes[Selected];
						return true;
					});
			if (Component.Shape == PulseForge::ColliderShape::Box && Inspector.Style.BeginPropertyRow("Half Extents"))
				DrawProperty(Inspector, Entity, PulseForge::PropertyComponentIds::Collider, PulseForge::PropertyIds::HalfExtents,
					[](PulseForge::PropertyValue& Value)
					{
						auto& Extents = std::get<glm::vec3>(Value);
						return ImGui::DragFloat3("##ColliderHalfExtents", &Extents.x, 0.01f, 0.001f, 1000000.0f, "%.2f");
					});
			if (Component.Shape == PulseForge::ColliderShape::Sphere && Inspector.Style.BeginPropertyRow("Radius"))
				DrawProperty(Inspector, Entity, PulseForge::PropertyComponentIds::Collider, PulseForge::PropertyIds::SphereRadius,
					[](PulseForge::PropertyValue& Value)
					{ return ImGui::DragFloat("##ColliderSphereRadius", &std::get<float>(Value), 0.01f, 0.001f, 1000000.0f, "%.2f"); });
			if (Component.Shape == PulseForge::ColliderShape::Capsule)
			{
				if (Inspector.Style.BeginPropertyRow("Radius"))
					DrawProperty(Inspector, Entity, PulseForge::PropertyComponentIds::Collider, PulseForge::PropertyIds::CapsuleRadius,
						[](PulseForge::PropertyValue& Value)
						{ return ImGui::DragFloat("##ColliderCapsuleRadius", &std::get<float>(Value), 0.01f, 0.001f, 1000000.0f, "%.2f"); });
				if (Inspector.Style.BeginPropertyRow("Total Height"))
					DrawProperty(Inspector, Entity, PulseForge::PropertyComponentIds::Collider, PulseForge::PropertyIds::CapsuleHeight,
						[](PulseForge::PropertyValue& Value)
						{ return ImGui::DragFloat("##ColliderCapsuleHeight", &std::get<float>(Value), 0.01f, 0.001f, 1000000.0f, "%.2f"); });
			}
			if (Inspector.Style.BeginPropertyRow("Sensor"))
				DrawProperty(Inspector, Entity, PulseForge::PropertyComponentIds::Collider, PulseForge::PropertyIds::IsSensor,
					[](PulseForge::PropertyValue& Value) { return ImGui::Checkbox("##ColliderSensor", &std::get<bool>(Value)); });
			if (Inspector.Style.BeginPropertyRow("Category"))
				DrawProperty(Inspector, Entity, PulseForge::PropertyComponentIds::Collider, PulseForge::PropertyIds::CollisionCategory,
					[](PulseForge::PropertyValue& Value)
					{ return ImGui::InputScalar("##ColliderCategory", ImGuiDataType_U32, &std::get<uint32_t>(Value)); });
			if (Inspector.Style.BeginPropertyRow("Mask"))
				DrawProperty(Inspector, Entity, PulseForge::PropertyComponentIds::Collider, PulseForge::PropertyIds::CollisionMask,
					[](PulseForge::PropertyValue& Value)
					{ return ImGui::InputScalar("##ColliderMask", ImGuiDataType_U32, &std::get<uint32_t>(Value)); });
			Inspector.Style.EndPropertyTable();
		}
		(void)Component;
		Inspector.Style.EndComponentBody();
	}

	void EditorInspectorPanel::DrawAudioSourceComponent(Context& Inspector, const PulseForge::Entity& Entity)
	{
		const auto Result = Entity.GetAudioSource();
		if (!Result || !Result->has_value()) return;

		PulseForge::AudioSourceComponent Component = **Result;
		const PulseForge::AssetID OriginalAudioAsset = Component.AudioAsset;
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
			static constexpr const char* BusNames[]{ "Music", "Effects", "UI" };
			int BusIndex = static_cast<int>(Component.Bus);
			if (BusIndex < 0 || BusIndex >= static_cast<int>(std::size(BusNames))) BusIndex = static_cast<int>(PulseForge::AudioBus::Effects);
			if (Inspector.Style.BeginPropertyRow("Bus") && ImGui::Combo("##AudioBus", &BusIndex, BusNames, static_cast<int>(std::size(BusNames))))
			{
				Component.Bus = static_cast<PulseForge::AudioBus>(BusIndex);
				Changed = true;
			}
			if (Inspector.Style.BeginPropertyRow("Looping")) Changed |= ImGui::Checkbox("##AudioLooping", &Component.Looping);
			if (Inspector.Style.BeginPropertyRow("Play On Start")) Changed |= ImGui::Checkbox("##AudioPlayOnStart", &Component.PlayOnStart);
			if (Inspector.Style.BeginPropertyRow("Spatialized")) Changed |= ImGui::Checkbox("##AudioSpatialized", &Component.Spatialized);
			Inspector.Style.EndPropertyTable();
		}
		if (Audio && *Audio != OriginalAudioAsset)
		{
			auto Stopped = Inspector.Actions.StopAudioPreview();
			if (!Stopped) Inspector.Actions.ReportInspectorError(Stopped.error());
		}
		if (!Audio) ImGui::BeginDisabled();
		if (ImGui::Button("Preview audio##AudioSource"))
		{
			if (Audio)
			{
				auto Previewed = Inspector.Actions.PreviewAudioAsset(*Audio, Component.Bus);
				if (!Previewed) Inspector.Actions.ReportInspectorError(Previewed.error());
			}
		}
		if (!Audio) ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::Button("Stop preview##AudioSource"))
		{
			auto Stopped = Inspector.Actions.StopAudioPreview();
			if (!Stopped) Inspector.Actions.ReportInspectorError(Stopped.error());
		}
		if (Changed && Audio)
		{
			Component.AudioAsset = *Audio;
			RecordComponentOperation(
				Inspector, [Entity, Component] { return Entity.SetAudioSource(Component); }, "Audio source update failed");
		}
		Inspector.Style.EndComponentBody();
	}

	void EditorInspectorPanel::DrawRuntimeUICanvasComponent(Context& Inspector, const PulseForge::Entity& Entity)
	{
		const auto Result = Entity.GetRuntimeUICanvas();
		if (!Result || !Result->has_value()) return;
		PulseForge::RuntimeUICanvasComponent Component = **Result;
		const bool Expanded = Inspector.Style.SectionHeader(PulseForgeEditor::EditorIcon::Image, "Runtime UI Canvas");
		ImGui::SameLine();
		ImGui::SetCursorPosX((std::max)(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - 31.0f));
		if (Inspector.Style.IconButton(PulseForgeEditor::EditorIcon::Delete, "Remove", "RemoveRuntimeUICanvas",
				"Remove Runtime UI Canvas component", true))
		{
			RecordComponentOperation(Inspector, [Entity] { return Entity.RemoveRuntimeUICanvas(); }, "Runtime UI canvas removal failed");
			return;
		}
		if (!Expanded) return;
		Inspector.Style.BeginComponentBody("RuntimeUICanvasComponentBody");
		std::optional<PulseForge::AssetID> Document = Component.DocumentAsset;
		int InputPolicy = static_cast<int>(Component.InputPolicy);
		const char* InputPolicies[]{ "Passive HUD", "Non-modal UI", "Modal menu" };
		bool Changed = false;
		if (Inspector.Style.BeginPropertyTable("RuntimeUICanvasProperties"))
		{
			if (Inspector.Style.BeginPropertyRow("UI Document"))
				Changed |= DrawAssetSelector(Inspector, "##RuntimeUIDocument", Document, { ".pfui" }, false);
			if (Inspector.Style.BeginPropertyRow("Enabled"))
				Changed |= ImGui::Checkbox("##RuntimeUICanvasEnabled", &Component.Enabled);
			if (Inspector.Style.BeginPropertyRow("Input Policy") && ImGui::Combo(
				"##RuntimeUICanvasInputPolicy", &InputPolicy, InputPolicies, 3))
			{
				Component.InputPolicy = static_cast<PulseForge::RuntimeUIInputPolicy>(InputPolicy);
				Changed = true;
			}
			Inspector.Style.EndPropertyTable();
		}
		if (Document) Component.DocumentAsset = *Document;
		if (Changed && Document)
			RecordComponentOperation(Inspector, [Entity, Component] { return Entity.SetRuntimeUICanvas(Component); },
				"Runtime UI canvas update failed");
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
		const PulseForge::AssetID ScriptAsset = Result->value().ScriptAsset;
		if (ImGui::SmallButton("Open Script Source"))
		{
			auto Opened = Inspector.Actions.OpenScriptAsset(ScriptAsset);
			if (!Opened) Inspector.Actions.ReportInspectorError("Could not open script source: " + Opened.error());
		}
		ImGui::SameLine();
		if (ImGui::SmallButton("Reload Script"))
		{
			m_CachedScriptMetadata.reset();
			auto Reloaded = Inspector.Actions.RequestScriptReload(Entity);
			if (!Reloaded) Inspector.Actions.ReportInspectorError("Could not request script reload: " + Reloaded.error());
		}
		const auto Diagnostics = Inspector.Actions.GetScriptDiagnostics(Entity.GetUUID(), ScriptAsset);
		for (size_t DiagnosticIndex = 0; DiagnosticIndex < Diagnostics.size(); ++DiagnosticIndex)
		{
			const PulseForge::ScriptDiagnostic& Diagnostic = Diagnostics[DiagnosticIndex];
			if (Diagnostic.Entity != Entity.GetUUID() || Diagnostic.ScriptAsset != ScriptAsset) continue;
			ImGui::PushID(static_cast<int>(DiagnosticIndex));
			std::string Label = ScriptDiagnosticName(Diagnostic.Code);
			if (!Diagnostic.SourceRelativePath.empty())
			{
				Label += " — " + Diagnostic.SourceRelativePath;
				if (Diagnostic.Line > 0) Label += ":" + std::to_string(Diagnostic.Line);
			}
			ImGui::TextColored(ImVec4(0.96f, 0.53f, 0.31f, 1.0f), "%s", Label.c_str());
			ImGui::TextWrapped("%s", Diagnostic.Message.c_str());
			ImGui::TextDisabled("Opens the current file for this asset; it may differ from the diagnostic's captured source. Line navigation is unavailable.");
			if (ImGui::SmallButton("Open script source for diagnostic"))
			{
				DispatchScriptDiagnosticOpen(Inspector.Actions, Diagnostic);
			}
			ImGui::PopID();
		}
		DrawScriptFloatProperties(Inspector, Entity);
		Inspector.Style.EndComponentBody();
	}

	void EditorInspectorPanel::DrawScriptFloatProperties(Context& Inspector, const PulseForge::Entity& Entity)
	{
		ImGui::TextDisabled("Script Properties");
		const auto ScriptResult = Entity.GetScript();
		if (!ScriptResult || !ScriptResult->has_value())
		{
			Inspector.Style.TextMuted("The Script component is unavailable.");
			return;
		}

		const PulseForge::ScriptComponent Script = **ScriptResult;
		if (ImGui::SmallButton("Refresh Script Declarations"))
			m_CachedScriptMetadata.reset();
		const PulseForge::UUID ProjectIdentity = Inspector.Actions.GetScriptProjectIdentity();
		if (!m_CachedScriptMetadata || !m_CachedScriptProject || *m_CachedScriptProject != ProjectIdentity ||
			!m_CachedScriptAsset || *m_CachedScriptAsset != Script.ScriptAsset)
		{
			m_CachedScriptProject = ProjectIdentity;
			m_CachedScriptAsset = Script.ScriptAsset;
			m_CachedScriptMetadata = Inspector.Actions.ReadScriptSourceMetadata(Script.ScriptAsset);
		}
		const auto& Metadata = *m_CachedScriptMetadata;
		if (!Metadata)
			ImGui::TextWrapped("Script declarations are unavailable: %s", Metadata.error().c_str());
		else if (!Metadata->HasDeclarations)
			Inspector.Style.TextMuted("This script has no declared Inspector properties.");

		std::vector<std::string> DeclaredIDs;
		const bool HasDeclaredRows = Metadata && !Metadata->FloatProperties.empty();
		if (HasDeclaredRows && Inspector.Style.BeginPropertyTable("ScriptFloatProperties"))
		{
			DeclaredIDs.reserve(Metadata->FloatProperties.size());
			for (const PulseForge::ScriptFloatPropertyDeclaration& Declaration : Metadata->FloatProperties)
			{
				DeclaredIDs.push_back(Declaration.ID);
				const auto Stored = Script.FloatProperties.find(Declaration.ID);
				const bool HasOverride = Stored != Script.FloatProperties.end();
				float Value = HasOverride ? Stored->second : Declaration.DefaultValue;
				const std::string Label = Declaration.Label + (HasOverride ? "  [Override]" : "  [Default]");
				ImGui::PushID(Declaration.ID.c_str());
				if (Inspector.Style.BeginPropertyRow(Label))
				{
					ImGui::SetNextItemWidth((std::max)(ImGui::GetContentRegionAvail().x, 1.0f));
					if (ImGui::DragFloat("##ScriptFloatProperty", &Value, 0.05f, Declaration.Minimum,
							Declaration.Maximum, "%.3f"))
					{
						auto Updated = Inspector.Actions.RunInspectorSceneEdit("Edit script property " + Declaration.ID,
							[Entity, Identifier = Declaration.ID, Value, Actions = &Inspector.Actions]()
								-> std::expected<void, PulseForge::SceneError>
							{
								const auto Written = PulseForge::WriteProperty(Entity, PulseForge::PropertyComponentIds::ScriptFloat,
									Identifier, PulseForge::PropertyValue{ Value });
								if (!Written) return std::unexpected(ToSceneError(Written.error()));
								if (Actions->IsLinkedPrefabEntity(Entity))
									if (auto Recorded = Actions->RecordPrefabPropertyOverride(
											Entity, PulseForge::PropertyComponentIds::ScriptFloat, Identifier);
									!Recorded)
									return std::unexpected(PulseForge::SceneError{
										PulseForge::SceneErrorCode::InvalidPrefabInstance, Recorded.error() });
								return {};
							});
						if (!Updated) Inspector.Actions.ReportInspectorError("Script property update failed: " + Updated.error());
					}
					if (HasOverride && ImGui::SmallButton("Reset"))
					{
						if (Inspector.Actions.IsLinkedPrefabEntity(Entity))
						{
							auto Reset = Inspector.Actions.ResetPrefabPropertyOverride(
								Entity, PulseForge::PropertyComponentIds::ScriptFloat, Declaration.ID);
							if (!Reset) Inspector.Actions.ReportInspectorError("Script property reset failed: " + Reset.error());
						}
						else
						{
							auto Reset = Inspector.Actions.RunInspectorSceneEdit("Reset script property " + Declaration.ID,
								[Entity, Identifier = Declaration.ID]() -> std::expected<void, PulseForge::SceneError>
								{
									const auto Removed = PulseForge::RemoveScriptFloatProperty(Entity, Identifier);
									if (!Removed) return std::unexpected(ToSceneError(Removed.error()));
									return {};
								});
							if (!Reset) Inspector.Actions.ReportInspectorError("Script property reset failed: " + Reset.error());
						}
					}
					if (HasOverride && (Value < Declaration.Minimum || Value > Declaration.Maximum))
						ImGui::TextColored(ImVec4(0.96f, 0.53f, 0.31f, 1.0f), "Stored value is outside the declared range.");
				}
				ImGui::PopID();
			}
			Inspector.Style.EndPropertyTable();
		}

		std::vector<std::pair<std::string, float>> LegacyValues;
		for (const auto& [Identifier, Value] : Script.FloatProperties)
		{
			if (std::find(DeclaredIDs.begin(), DeclaredIDs.end(), Identifier) == DeclaredIDs.end())
				LegacyValues.emplace_back(Identifier, Value);
		}
		if (!LegacyValues.empty())
		{
			ImGui::Spacing();
			ImGui::TextDisabled("Legacy values without declarations are preserved below.");
			if (Inspector.Style.BeginPropertyTable(HasDeclaredRows ? "LegacyScriptFloatProperties" : "ScriptFloatProperties"))
			{
				for (const auto& [Identifier, StoredValue] : LegacyValues)
				{
					ImGui::PushID(Identifier.c_str());
					if (Inspector.Style.BeginPropertyRow(Identifier + "  [Legacy]"))
					{
						float Value = StoredValue;
						ImGui::SetNextItemWidth((std::max)(ImGui::GetContentRegionAvail().x, 1.0f));
						if (ImGui::DragFloat("##LegacyScriptFloat", &Value, 0.05f,
								-PulseForge::MaximumScriptFloatPropertyMagnitude,
								PulseForge::MaximumScriptFloatPropertyMagnitude, "%.3f"))
						{
							auto Updated = Inspector.Actions.RunInspectorSceneEdit("Edit legacy script property " + Identifier,
								[Entity, Identifier, Value, Actions = &Inspector.Actions]() -> std::expected<void, PulseForge::SceneError>
								{
									const auto Written = PulseForge::WriteProperty(Entity, PulseForge::PropertyComponentIds::ScriptFloat,
										Identifier, PulseForge::PropertyValue{ Value });
									if (!Written) return std::unexpected(ToSceneError(Written.error()));
									if (Actions->IsLinkedPrefabEntity(Entity))
										if (auto Recorded = Actions->RecordPrefabPropertyOverride(
												Entity, PulseForge::PropertyComponentIds::ScriptFloat, Identifier);
										!Recorded)
										return std::unexpected(PulseForge::SceneError{
											PulseForge::SceneErrorCode::InvalidPrefabInstance, Recorded.error() });
									return {};
								});
							if (!Updated) Inspector.Actions.ReportInspectorError("Legacy script property update failed: " + Updated.error());
						}
						ImGui::SameLine();
						if (Inspector.Actions.IsLinkedPrefabEntity(Entity)) ImGui::BeginDisabled();
						const bool Remove = ImGui::SmallButton("Remove");
						if (Inspector.Actions.IsLinkedPrefabEntity(Entity)) ImGui::EndDisabled();
						if (Remove && !Inspector.Actions.IsLinkedPrefabEntity(Entity))
						{
							auto Removed = Inspector.Actions.RunInspectorSceneEdit("Remove legacy script property " + Identifier,
								[Entity, Identifier]() -> std::expected<void, PulseForge::SceneError>
								{
									const auto Result = PulseForge::RemoveScriptFloatProperty(Entity, Identifier);
									if (!Result) return std::unexpected(ToSceneError(Result.error()));
									return {};
								});
							if (!Removed) Inspector.Actions.ReportInspectorError("Legacy script property removal failed: " + Removed.error());
						}
					}
					ImGui::PopID();
				}
				Inspector.Style.EndPropertyTable();
			}
		}
	}

} // namespace PulseForgeEditor
