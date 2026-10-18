#include "Editor/EditorContentBrowserAssetView.h"

#include "Assets/AssetTypes.h"
#include "Assets/AssetPathResolver.h"
#include "Assets/Project.h"
#include "Editor/EditorAssetPreviewPolicy.h"
#include "Editor/EditorAssetPreviewService.h"
#include "Editor/EditorMaterialEditorPolicy.h"
#include "Editor/EditorStyle.h"

namespace PulseForgeEditor
{
	void EditorContentBrowserAssetView::HandleAssetItem(EditorContentBrowserState& State, const PulseForge::Project* Project,
		const PulseForge::AssetRecord& Asset, const EditorContentBrowserAssetPresentation& Presentation, bool Activated,
		bool SceneAvailable, EditorStyle& Style, const EditorContentBrowserAssetActionHandler& ActionHandler,
		std::function<void()>& DeferredAction)
	{
		if (Activated)
		{
			State.SelectedAsset() = Asset.ID;
			switch (ResolveAssetBrowserOpenAction(Presentation.Extension, Activated, ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)))
			{
			case AssetBrowserOpenAction::OpenScene:
				ActionHandler(EditorContentBrowserAssetAction::OpenScene, Asset.ID);
				break;
			case AssetBrowserOpenAction::ShowMaterialDetails:
			{
				const auto Handler = ActionHandler;
				const PulseForge::AssetID Identifier = Asset.ID;
				DeferredAction = [Handler, Identifier] { Handler(EditorContentBrowserAssetAction::ShowMaterialDetails, Identifier); };
				break;
			}
			case AssetBrowserOpenAction::None:
				break;
			}
		}

		if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
		{
			const std::string Identifier = Asset.ID.ToString();
			ImGui::SetDragDropPayload("PF_MANAGED_ASSET_MOVE", Identifier.c_str(), Identifier.size() + 1);
			ImGui::Text("Move asset: %s", Presentation.Name.c_str());
			ImGui::EndDragDropSource();
		}
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\nUUID: %s", Presentation.FullPath.c_str(), Presentation.Identifier.c_str());
		if (!ImGui::BeginPopupContextItem("Asset Actions")) return;

		State.SelectedAsset() = Asset.ID;
		const PulseForge::AssetID Identifier = Asset.ID;
		const auto Defer = [&DeferredAction, &ActionHandler, &Identifier](EditorContentBrowserAssetAction Action)
		{
			const auto Handler = ActionHandler;
			DeferredAction = [Handler, Action, Identifier] { Handler(Action, Identifier); };
		};
		if (PulseForge::HasAssetTypeExtension(Asset.ProjectRelativePath, PulseForge::AssetFileType::Material) &&
			Style.MenuItem(EditorIcon::Material, "Show Material Details"))
			Defer(EditorContentBrowserAssetAction::ShowMaterialDetails);
		if (Style.MenuItem(EditorIcon::Rename, "Move / Rename...")) Defer(EditorContentBrowserAssetAction::Move);
		if (Style.MenuItem(EditorIcon::Duplicate, "Duplicate...")) Defer(EditorContentBrowserAssetAction::Duplicate);
		if (PulseForge::HasAssetTypeExtension(Asset.ProjectRelativePath, PulseForge::AssetFileType::Prefab) &&
			Style.MenuItem(EditorIcon::Cube, "Instantiate in Scene", nullptr, false, SceneAvailable))
			Defer(EditorContentBrowserAssetAction::InstantiatePrefab);
		const bool IsStartupScene = Project && Project->GetDescription().StartScene && *Project->GetDescription().StartScene == Identifier;
		if (PulseForge::HasAssetTypeExtension(Asset.ProjectRelativePath, PulseForge::AssetFileType::Scene) &&
			ImGui::MenuItem("Set as Startup Scene", nullptr, IsStartupScene, !IsStartupScene))
			Defer(EditorContentBrowserAssetAction::SetStartupScene);
		if (Style.MenuItem(EditorIcon::Delete, "Delete...")) Defer(EditorContentBrowserAssetAction::Delete);
		ImGui::EndPopup();
	}

	void EditorContentBrowserAssetView::DrawList(EditorContentBrowserState& State, const PulseForge::Project* Project, bool SceneAvailable,
		EditorStyle& Style, EditorContentBrowserAssetActionHandler ActionHandler, std::function<void()>& DeferredAction)
	{
		if (State.FilteredAssetIndices().empty()) return;

		if (!ImGui::BeginTable("ManagedAssetList", 2,
				ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
					ImGuiTableFlags_NoSavedSettings))
			return;
		ImGui::TableSetupColumn("Asset", ImGuiTableColumnFlags_WidthStretch, 1.0f);
		ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 72.0f);
		ImGuiListClipper Clipper;
		Clipper.Begin(static_cast<int>(State.FilteredAssetIndices().size()));
		while (Clipper.Step())
		{
			for (int Row = Clipper.DisplayStart; Row < Clipper.DisplayEnd; ++Row)
			{
				const size_t AssetIndex = State.FilteredAssetIndices()[static_cast<size_t>(Row)];
				const PulseForge::AssetRecord& Asset = State.Assets()[AssetIndex];
				const EditorContentBrowserAssetPresentation& Presentation = State.AssetPresentations()[AssetIndex];
				ImGui::PushID(Presentation.Identifier.c_str());
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				const bool IsSelected = State.SelectedAsset() && *State.SelectedAsset() == Asset.ID;
				const bool Activated =
					Style.Selectable(Presentation.Icon, Presentation.Name, IsSelected, ImGuiSelectableFlags_AllowDoubleClick);
				HandleAssetItem(State, Project, Asset, Presentation, Activated, SceneAvailable, Style, ActionHandler, DeferredAction);
				ImGui::TableSetColumnIndex(1);
				Style.TextMuted(Presentation.Extension.empty() ? "File" : std::string_view(Presentation.Extension).substr(1));
				ImGui::PopID();
			}
		}
		ImGui::EndTable();
	}

	void EditorContentBrowserAssetView::DrawGrid(EditorContentBrowserState& State, const PulseForge::Project* Project, bool SceneAvailable,
		EditorAssetPreviewService& Previews, EditorStyle& Style, EditorContentBrowserAssetActionHandler ActionHandler,
		std::function<void()>& DeferredAction)
	{
		if (State.FilteredAssetIndices().empty()) return;

#ifdef PF_EDITOR_RENDERER_OPENGL
		(void)Previews;
#endif

		const float AvailableWidth = ImGui::GetContentRegionAvail().x;
		const float DesiredTileWidth = 124.0f;
		const float Spacing = ImGui::GetStyle().ItemSpacing.x;
		const int ColumnCount = (std::max)(1, static_cast<int>((AvailableWidth + Spacing) / (DesiredTileWidth + Spacing)));
		const int RowCount = static_cast<int>(
			(State.FilteredAssetIndices().size() + static_cast<size_t>(ColumnCount) - 1) / static_cast<size_t>(ColumnCount));
		if (!ImGui::BeginTable("ManagedAssetGrid", ColumnCount,
				ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_NoBordersInBody))
			return;

		constexpr float TileHeight = 102.0f;
		ImGuiListClipper Clipper;
		Clipper.Begin(RowCount, TileHeight + Spacing);
		while (Clipper.Step())
		{
			for (int Row = Clipper.DisplayStart; Row < Clipper.DisplayEnd; ++Row)
			{
				ImGui::TableNextRow(ImGuiTableRowFlags_None, TileHeight);
				for (int Column = 0; Column < ColumnCount; ++Column)
				{
					const size_t AssetIndex = static_cast<size_t>(Row) * static_cast<size_t>(ColumnCount) + static_cast<size_t>(Column);
					if (AssetIndex >= State.FilteredAssetIndices().size()) break;
					ImGui::TableSetColumnIndex(Column);
					const size_t SourceIndex = State.FilteredAssetIndices()[AssetIndex];
					const PulseForge::AssetRecord& Asset = State.Assets()[SourceIndex];
					const EditorContentBrowserAssetPresentation& Presentation = State.AssetPresentations()[SourceIndex];
					ImGui::PushID(Presentation.Identifier.c_str());
					const bool IsSelected = State.SelectedAsset() && *State.SelectedAsset() == Asset.ID;
					uint64_t PreviewID = 0;
					uint32_t PreviewWidth = 0;
					uint32_t PreviewHeight = 0;
#ifndef PF_EDITOR_RENDERER_OPENGL
					const bool IsImagePreview = IsSupportedImagePreviewExtension(Asset.ProjectRelativePath);
					const bool IsMaterialPreview =
						PulseForge::HasAssetTypeExtension(Asset.ProjectRelativePath, PulseForge::AssetFileType::Material);
					if (Project && (IsImagePreview || IsMaterialPreview))
					{
						const std::string ProjectID = Project->GetDescription().ID.ToString();
						const std::string AssetID = Asset.ID.ToString();
						if (!Previews.TouchPreview(ProjectID, AssetID))
						{
							auto SourcePath =
								PulseForge::AssetPathResolver::ResolveManagedSourcePath(Project->GetRootPath(), Asset.ProjectRelativePath);
							if (SourcePath)
							{
								if (IsMaterialPreview) Previews.RequestMaterialSwatch(ProjectID, AssetID, *SourcePath);
								else Previews.RequestImage(ProjectID, AssetID, *SourcePath);
							}
							else Previews.MarkUnavailable(ProjectID, AssetID);
						}
						if (const auto Preview = Previews.FindPreview(ProjectID, AssetID))
						{
							PreviewID = Preview->TextureID;
							PreviewWidth = Preview->Width;
							PreviewHeight = Preview->Height;
						}
					}
#endif
					const bool Activated = Style.AssetTile(Presentation.Icon, Presentation.Name, IsSelected,
						ImVec2((std::max)(ImGui::GetContentRegionAvail().x - 2.0f, 1.0f), TileHeight),
						ImGuiSelectableFlags_AllowDoubleClick, PreviewID, PreviewWidth, PreviewHeight);
					HandleAssetItem(State, Project, Asset, Presentation, Activated, SceneAvailable, Style, ActionHandler, DeferredAction);
					ImGui::PopID();
				}
			}
		}
		ImGui::EndTable();
	}
} // namespace PulseForgeEditor
