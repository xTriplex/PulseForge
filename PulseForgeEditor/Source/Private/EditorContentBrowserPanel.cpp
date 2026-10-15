#include "Editor/EditorContentBrowserPanel.h"

#include "Assets/AssetPathResolver.h"
#include "Assets/Project.h"
#include "Editor/EditorAssetPreviewService.h"
#include "Editor/EditorContentBrowserPolicy.h"
#include "Editor/EditorStyle.h"
#include "Editor/EditorWorkspaceView.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <functional>
#include <string>
#include <string_view>

namespace PulseForgeEditor
{
	namespace
	{
		std::string PathToUtf8(const std::filesystem::path& Path)
		{
			const std::u8string Encoded = Path.generic_u8string();
			return { reinterpret_cast<const char*>(Encoded.data()), Encoded.size() };
		}

		std::string FitEditorContextLabel(std::string_view Label, float AvailableWidth)
		{
			constexpr std::string_view Ellipsis = "\xe2\x80\xa6";
			if (ImGui::CalcTextSize(Label.data(), Label.data() + Label.size()).x <= AvailableWidth)
				return std::string(Label);

			size_t PrefixLength = Utf8SafePrefixLength(Label, 480);
			const float EllipsisWidth = ImGui::CalcTextSize(Ellipsis.data(), Ellipsis.data() + Ellipsis.size()).x;
			while (PrefixLength > 0)
			{
				const std::string_view Prefix = Label.substr(0, PrefixLength);
				if (ImGui::CalcTextSize(Prefix.data(), Prefix.data() + Prefix.size()).x + EllipsisWidth <= AvailableWidth)
					return std::string(Prefix) + std::string(Ellipsis);
				PrefixLength = Utf8SafePrefixLength(Label, PrefixLength - 1);
			}
			return EllipsisWidth <= AvailableWidth ? std::string(Ellipsis) : std::string{};
		}
	}

	void EditorContentBrowserPanel::DrawContents(
		EditorContentBrowserState& State,
		const PulseForge::Project& Project,
		bool SceneAvailable,
		EditorAssetPreviewService& Previews,
		EditorStyle& Style,
		EditorContentBrowserPanelActions& Actions)
	{
		std::function<void()> DeferredAction;
		const EditorContentBrowserAssetActionHandler AssetActionHandler =
			[&Actions](EditorContentBrowserAssetAction Action, const PulseForge::AssetID& Identifier)
			{
				Actions.HandleContentBrowserAssetAction(Action, Identifier);
			};
		const ImVec2 Available = ImGui::GetContentRegionAvail();
		const ContentBrowserHorizontalLayout HorizontalLayout = GetContentBrowserHorizontalLayout(Available.x);
		if (HorizontalLayout.ShowFolderTree)
		{
			ImGui::BeginChild("ContentFolderTree",
				ImVec2(HorizontalLayout.FolderTreeWidth, 0.0f), ImGuiChildFlags_Borders);
			m_Navigation.DrawFolderTree(State, Style, Actions, DeferredAction);
			ImGui::EndChild();
			ImGui::SameLine(0.0f, 6.0f);
		}

		ImGui::BeginChild("ContentAssetArea", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
		if (HorizontalLayout.ShowFolderTree)
			m_Navigation.DrawBreadcrumbs(State, Style);
		else
			m_Navigation.DrawCompactFolderPicker(State);

		if (Style.BeginToolbar("ContentBrowserToolbar", EditorStyleMetrics::PanelToolbarHeight))
		{
			const bool Compact = ImGui::GetWindowWidth() < 440.0f;
			if (Compact
				? Style.IconButton(EditorIcon::Import, "Import", "ImportAsset", "Import asset into the project")
				: Style.SmallButton(EditorIcon::Import, "Import"))
				Actions.HandleContentBrowserToolbarAction(EditorContentBrowserToolbarAction::Import);
			ImGui::SameLine();
			if (Compact
				? Style.IconButton(EditorIcon::Refresh, "Refresh", "RefreshAssets",
					"Refresh the managed asset registry")
				: Style.SmallButton(EditorIcon::Refresh, "Refresh"))
				Actions.HandleContentBrowserToolbarAction(EditorContentBrowserToolbarAction::Refresh);
			ImGui::SameLine();
			const float SearchWidth = (std::max)(ImGui::GetContentRegionAvail().x - 83.0f, 52.0f);
			ImGui::SetNextItemWidth(SearchWidth);
			if (m_FocusSearchNextFrame)
			{
				ImGui::SetKeyboardFocusHere();
				m_FocusSearchNextFrame = false;
			}
			ImGui::InputTextWithHint("##AssetSearch", "Search assets...", &State.SearchText());
			ImGui::SameLine();
			if (Style.ToolIconButton(EditorIcon::Grid, "Grid", "AssetGridMode", "Grid view", State.GridView()))
				State.GridView() = true;
			ImGui::SameLine();
			if (Style.ToolIconButton(EditorIcon::List, "List", "AssetListMode", "List view", !State.GridView()))
				State.GridView() = false;
		}
		Style.EndToolbar();

		State.RebuildFilteredAssetIndices();
		const EditorContentFolderNode* CurrentFolderNode = State.FindFolder(State.CurrentFolder());
		const bool ShowFolderTiles = State.SearchText().empty() && CurrentFolderNode &&
			!CurrentFolderNode->Children.empty();
		const std::vector<EditorContentFolderNode>* VisibleFolders =
			ShowFolderTiles ? &CurrentFolderNode->Children : nullptr;
		bool SelectedAssetVisible = false;
		bool SelectedMaterial = false;
		if (State.SelectedAsset())
		{
			for (const size_t Index : State.FilteredAssetIndices())
				if (State.Assets()[Index].ID == *State.SelectedAsset())
				{
					SelectedAssetVisible = true;
					SelectedMaterial = State.AssetPresentations()[Index].Extension == ".material";
					break;
				}
		}
		const ContentBrowserVerticalLayout VerticalLayout = GetContentBrowserVerticalLayout(
			ImGui::GetContentRegionAvail().y,
			SelectedAssetVisible,
			SelectedMaterial,
			m_DetailsExpandedCompact);
		ImGui::BeginChild("ContentAssetItems", ImVec2(0.0f, VerticalLayout.AssetItemsHeight), ImGuiChildFlags_None);
		if (VisibleFolders && !VisibleFolders->empty())
		{
			m_Navigation.DrawFolderItems(State, *VisibleFolders, Style, Actions, DeferredAction);
			if (!State.FilteredAssetIndices().empty())
				ImGui::Separator();
		}
		if (State.GridView())
			m_AssetView.DrawGrid(State, &Project, SceneAvailable, Previews, Style, AssetActionHandler, DeferredAction);
		else
			m_AssetView.DrawList(State, &Project, SceneAvailable, Style, AssetActionHandler, DeferredAction);
		if ((!VisibleFolders || VisibleFolders->empty()) && State.FilteredAssetIndices().empty())
			Style.EmptyState(EditorIcon::Search,
				State.Assets().empty() ? "The project has no managed assets." :
				"No assets or folders match this location.");
		ImGui::EndChild();

		const bool HasDeferredAction = static_cast<bool>(DeferredAction);
		if (DeferredAction)
			DeferredAction();
		if (!HasDeferredAction && SelectedAssetVisible && VerticalLayout.SelectionSummaryHeight > 0.0f)
		{
			const auto SelectedRecord = Project.GetAssetRegistry().Find(*State.SelectedAsset());
			std::string SelectedName = SelectedRecord
				? PathToUtf8(SelectedRecord->ProjectRelativePath.filename()) : "Selected asset";
			if (SelectedMaterial && Actions.IsContentBrowserMaterialDraftDirty(*State.SelectedAsset()))
				SelectedName += " \xE2\x80\x94 Unsaved draft";
			const float ActionWidth = SelectedMaterial ? 154.0f : 112.0f;
			const float SummaryWidth = (std::max)(
				ImGui::GetContentRegionAvail().x - ActionWidth - ImGui::GetStyle().ItemSpacing.x, 20.0f);
			const std::string Summary = FitEditorContextLabel(SelectedName, SummaryWidth);
			ImGui::TextUnformatted(Summary.c_str());
			if (ImGui::IsItemHovered() && SelectedRecord)
			{
				const std::string FullPath = PathToUtf8(SelectedRecord->ProjectRelativePath);
				const std::string Identifier = SelectedRecord->ID.ToString();
				ImGui::SetTooltip("%s\nUUID: %s", FullPath.c_str(), Identifier.c_str());
			}
			ImGui::SameLine();
			if (SelectedMaterial)
			{
				if (ImGui::Button("Open Material Details", ImVec2(ActionWidth, 0.0f)))
					Actions.HandleContentBrowserAssetAction(
						EditorContentBrowserAssetAction::ShowMaterialDetails, *State.SelectedAsset());
			}
			else if (ImGui::SmallButton(m_DetailsExpandedCompact ? "Hide Details" : "Details"))
				m_DetailsExpandedCompact = !m_DetailsExpandedCompact;
		}
		if (!HasDeferredAction && SelectedAssetVisible && VerticalLayout.DetailsHeight > 0.0f)
		{
			ImGui::BeginChild("SelectedAssetDetails", ImVec2(0.0f, VerticalLayout.DetailsHeight),
				ImGuiChildFlags_Borders, ImGuiWindowFlags_AlwaysVerticalScrollbar);
			DrawSelectedAssetDetails(State, Project, SceneAvailable, Style, Actions);
			ImGui::EndChild();
		}
		ImGui::EndChild();
	}

	void EditorContentBrowserPanel::DrawSelectedAssetDetails(
		EditorContentBrowserState& State,
		const PulseForge::Project& Project,
		bool SceneAvailable,
		EditorStyle& Style,
		EditorContentBrowserPanelActions& Actions)
	{
		if (!State.SelectedAsset())
			return;
		const auto RegisteredAsset = Project.GetAssetRegistry().Find(*State.SelectedAsset());
		if (!RegisteredAsset)
		{
			State.SelectedAsset().reset();
			return;
		}
		const auto AssetIterator = std::find_if(State.Assets().begin(), State.Assets().end(),
			[&RegisteredAsset](const PulseForge::AssetRecord& Asset) { return Asset.ID == RegisteredAsset->ID; });
		if (AssetIterator == State.Assets().end())
			return;

		const size_t AssetIndex = static_cast<size_t>(std::distance(State.Assets().begin(), AssetIterator));
		if (AssetIndex >= State.AssetPresentations().size())
			return;
		const PulseForge::AssetRecord& Asset = *RegisteredAsset;
		const EditorContentBrowserAssetPresentation& Presentation = State.AssetPresentations()[AssetIndex];
		if (Presentation.Extension == ".material")
			return;

		ImGui::Separator();
		{
			[[maybe_unused]] const auto Emphasis = Style.PushEmphasisFont();
			Style.IconText(GetAssetBrowserIcon(Asset.ProjectRelativePath), Presentation.Name);
		}
		ImGui::PushStyleColor(ImGuiCol_Text, Style.GetColor(EditorColorToken::TextMuted));
		ImGui::TextWrapped("%s", Presentation.FullPath.c_str());
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s\nUUID: %s", Presentation.FullPath.c_str(), Presentation.Identifier.c_str());
		ImGui::PopStyleColor();

		if (Presentation.Extension == ".scene")
		{
			const bool IsStartupScene = Project.GetDescription().StartScene &&
				*Project.GetDescription().StartScene == Asset.ID;
			if (IsStartupScene)
				Style.TextMuted("Project startup scene");
			else if (Style.SmallButton(EditorIcon::Settings, "Set as Startup Scene"))
				Actions.HandleContentBrowserAssetAction(EditorContentBrowserAssetAction::SetStartupScene, Asset.ID);
		}
		if (Style.IconButton(EditorIcon::Rename, "Move", "SelectedAssetMove", "Move or rename selected asset"))
			Actions.HandleContentBrowserAssetAction(EditorContentBrowserAssetAction::Move, Asset.ID);
		ImGui::SameLine();
		if (Style.IconButton(EditorIcon::Duplicate, "Duplicate", "SelectedAssetDuplicate",
			"Duplicate selected asset"))
			Actions.HandleContentBrowserAssetAction(EditorContentBrowserAssetAction::Duplicate, Asset.ID);
		ImGui::SameLine();
		const bool IsProtectedScene = Actions.IsProtectedSceneAsset(Asset.ID);
		ImGui::BeginDisabled(IsProtectedScene);
		if (Style.IconButton(EditorIcon::Delete, "Delete", "SelectedAssetDelete", "Delete selected asset", true))
			Actions.HandleContentBrowserAssetAction(EditorContentBrowserAssetAction::Delete, Asset.ID);
		ImGui::EndDisabled();
		if (Presentation.Extension == ".prefab")
		{
			ImGui::SameLine();
			ImGui::BeginDisabled(!SceneAvailable);
			if (Style.SmallButton(EditorIcon::Cube, "Instantiate"))
				Actions.HandleContentBrowserAssetAction(EditorContentBrowserAssetAction::InstantiatePrefab, Asset.ID);
			ImGui::EndDisabled();
		}
		if (IsProtectedScene)
			Style.TextMuted("Open and startup scenes cannot be deleted.");
	}
}
