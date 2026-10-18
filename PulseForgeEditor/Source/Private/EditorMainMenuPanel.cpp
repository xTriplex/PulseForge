#include "Editor/EditorMainMenuPanel.h"

#include "Assets/AssetTypes.h"

#include "Editor/EditorStyle.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <utility>

namespace PulseForgeEditor
{
	namespace
	{
		std::string PathToUtf8(const std::filesystem::path& Path)
		{
			const std::u8string Encoded = Path.u8string();
			return { reinterpret_cast<const char*>(Encoded.data()), Encoded.size() };
		}

	} // namespace

	EditorMainMenuDrawResult EditorMainMenuPanel::Draw(const EditorMainMenuState& State, const EditorStyle& Style) const
	{
		EditorMainMenuDrawResult Result;
		if (!ImGui::BeginMainMenuBar()) return Result;
		const ImGuiViewport* WindowViewport = ImGui::GetWindowViewport();
		const ImGuiViewport* MainViewport = ImGui::GetMainViewport();
		Result.FocusMainDocumentHost = WindowViewport && MainViewport && WindowViewport->ID == MainViewport->ID &&
									   ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

		const auto Submit = [&Result, &State](EditorMainMenuActionKind Kind)
		{
			if (!Result.Action) Result.Action = MakeEditorMainMenuAction(Kind, State.FocusedCommandTarget);
		};
		const auto SubmitScene = [&Result, &State](EditorMainMenuActionKind Kind)
		{
			if (!Result.Action) Result.Action = MakeEditorMainMenuAction(Kind, State.LevelDocumentTarget);
		};
		if (ImGui::BeginMenu("File"))
		{
			if (Style.MenuItem(EditorIcon::NewDocument, "New Project...")) Submit(EditorMainMenuActionKind::NewProject);
			if (Style.MenuItem(EditorIcon::FolderOpen, "Open Project...")) Submit(EditorMainMenuActionKind::OpenProject);
			if (State.ProjectOpen && Style.MenuItem(EditorIcon::Close, "Close Project")) Submit(EditorMainMenuActionKind::CloseProject);
			ImGui::Separator();
			if (Style.MenuItem(EditorIcon::Close, "Exit")) Submit(EditorMainMenuActionKind::Exit);
			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu("Edit", State.EditMenuEnabled))
		{
			if (ImGui::MenuItem(State.UndoLabel.c_str(), "Ctrl+Z", false, State.CanUndo)) Submit(EditorMainMenuActionKind::Undo);
			if (ImGui::MenuItem(State.RedoLabel.c_str(), "Ctrl+Y / Ctrl+Shift+Z", false, State.CanRedo))
				Submit(EditorMainMenuActionKind::Redo);
			ImGui::Separator();
			if (ImGui::MenuItem("Cut", "Ctrl+X", false, State.CanEditScene && State.HasSelection)) Submit(EditorMainMenuActionKind::Cut);
			if (ImGui::MenuItem("Copy", "Ctrl+C", false, State.CanEditScene && State.HasSelection)) Submit(EditorMainMenuActionKind::Copy);
			if (ImGui::MenuItem("Paste", "Ctrl+V", false, State.CanPaste)) Submit(EditorMainMenuActionKind::Paste);
			if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, State.CanEditScene && State.HasSelection))
				Submit(EditorMainMenuActionKind::Duplicate);
			if (ImGui::MenuItem("Delete", "Delete", false, State.CanEditScene && State.HasSelection))
				Submit(EditorMainMenuActionKind::Delete);
			ImGui::Separator();
			if (ImGui::MenuItem("Select All", "Ctrl+A", false, State.CanEditScene && State.HasSceneEntities))
				Submit(EditorMainMenuActionKind::SelectAll);
			if (ImGui::MenuItem("Rename", "F2", false, State.CanEditScene && State.CanRename)) Submit(EditorMainMenuActionKind::Rename);
			if (ImGui::MenuItem("Focus Selected", "F", false, State.CanEditScene && State.CanFocusSelection))
				Submit(EditorMainMenuActionKind::FocusSelection);
			if (ImGui::MenuItem("Snap to Supporting Surface", "End", false, State.CanEditScene && State.CanSnapSelection))
				Submit(EditorMainMenuActionKind::SnapSelection);
			if (ImGui::MenuItem("Hide Selected", "H", false, State.CanEditScene && State.CanHideSelection))
				Submit(EditorMainMenuActionKind::HideSelection);
			if (ImGui::MenuItem("Show All", "Ctrl+H", false, State.CanEditScene && State.CanShowAll))
				Submit(EditorMainMenuActionKind::ShowAll);
			if (ImGui::MenuItem("Isolate Selection", "Shift+H", false, State.CanEditScene && State.CanIsolateSelection))
				Submit(EditorMainMenuActionKind::IsolateSelection);
			if (ImGui::MenuItem("Restore Isolation Visibility", nullptr, false, State.CanEditScene && State.CanRestoreIsolation))
				Submit(EditorMainMenuActionKind::RestoreIsolation);
			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu("Scene", State.ProjectOpen))
		{
			if (Style.MenuItem(EditorIcon::NewDocument, "New Scene...")) SubmitScene(EditorMainMenuActionKind::NewScene);
			if (Style.MenuItem(EditorIcon::Save, "Save Scene", nullptr, false, State.CanSaveScene))
				SubmitScene(EditorMainMenuActionKind::SaveScene);
			if (Style.MenuItem(EditorIcon::Save, "Save Scene As...", nullptr, false, State.CanSaveSceneAs))
				SubmitScene(EditorMainMenuActionKind::SaveSceneAs);
			if (ImGui::BeginMenu("Open Scene", State.ProjectOpen))
			{
				bool HasScenes = false;
				for (const PulseForge::AssetRecord& Asset : State.Assets)
				{
					if (!PulseForge::HasAssetTypeExtension(Asset.ProjectRelativePath, PulseForge::AssetFileType::Scene)) continue;
					HasScenes = true;
					const bool IsOpen = State.ActiveSceneAsset && *State.ActiveSceneAsset == Asset.ID;
					const std::string Label = PathToUtf8(Asset.ProjectRelativePath.filename());
					if (ImGui::MenuItem(Label.c_str(), nullptr, IsOpen))
					{
						if (!Result.Action)
							Result.Action = MakeEditorMainMenuAction(EditorMainMenuActionKind::OpenScene, State.LevelDocumentTarget);
						if (Result.Action && Result.Action->Kind == EditorMainMenuActionKind::OpenScene) Result.Action->Asset = Asset.ID;
					}
				}
				if (!HasScenes) ImGui::MenuItem("No scene assets", nullptr, false, false);
				ImGui::EndMenu();
			}
			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu("Window"))
		{
			for (const EditorPanelDescriptor& Descriptor : EditorPanelDescriptors)
			{
				const bool Visible = State.PanelVisibility[static_cast<size_t>(Descriptor.Panel)];
				const std::string_view Label =
					Descriptor.Panel == EditorPanel::MaterialDetails ? "Material Details" : Descriptor.WindowName;
				if (ImGui::MenuItem(Label.data(), nullptr, Visible) && !Result.Action)
					Result.Action = EditorMainMenuAction{
						.Kind = EditorMainMenuActionKind::SetPanelVisible, .Panel = Descriptor.Panel, .PanelVisible = !Visible
					};
			}
			ImGui::Separator();
			if (ImGui::BeginMenu("New asset documents open in"))
			{
				constexpr std::array<std::pair<std::string_view, EditorDocumentOpenPreference>, 3> Preferences = {
					{ { "New native window", EditorDocumentOpenPreference::NewNativeWindow },
						{ "Main document strip", EditorDocumentOpenPreference::MainDocumentStrip },
						{ "Remembered placement", EditorDocumentOpenPreference::RememberedPlacement } }
				};
				for (const auto& [Label, Preference] : Preferences)
					if (ImGui::MenuItem(Label.data(), nullptr, State.OpeningPreference == Preference) && !Result.Action)
						Result.Action =
							EditorMainMenuAction{ .Kind = EditorMainMenuActionKind::SetOpeningPreference, .OpeningPreference = Preference };
				ImGui::EndMenu();
			}
			ImGui::Separator();
			if (ImGui::MenuItem("Reset Layout") && !Result.Action) Submit(EditorMainMenuActionKind::ResetLayout);
			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu("Help"))
		{
			if (ImGui::BeginMenu("Keyboard Shortcuts"))
			{
				ImGui::MenuItem("Select / Move / Rotate / Scale", "Q / W / E / R", false, false);
				ImGui::MenuItem("Focus selection / Snap to surface", "F / End", false, false);
				ImGui::MenuItem("Undo / Redo", "Ctrl+Z / Ctrl+Y", false, false);
				ImGui::MenuItem("Copy / Cut / Paste / Duplicate", "Ctrl+C / X / V / D", false, false);
				ImGui::MenuItem("Delete / Rename / Create entity", "Delete / F2 / Ctrl+Shift+N", false, false);
				ImGui::MenuItem("Hide / Isolate / Show all", "H / Shift+H / Ctrl+H", false, false);
				ImGui::MenuItem("Save / Save Scene As", "Ctrl+S / Ctrl+Shift+S", false, false);
				ImGui::MenuItem("Viewport orbit / pan / dolly", "Alt+LMB / MMB / RMB drag", false, false);
				ImGui::MenuItem("Duplicate while moving or rotating", "Alt+gizmo drag", false, false);
				ImGui::EndMenu();
			}
			ImGui::EndMenu();
		}
		ImGui::EndMainMenuBar();
		return Result;
	}
} // namespace PulseForgeEditor
