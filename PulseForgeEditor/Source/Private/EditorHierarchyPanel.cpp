#include "Editor/EditorHierarchyPanel.h"

#include "Editor/EditorEntityIconPolicy.h"
#include "Editor/EditorHierarchyPanelPolicy.h"
#include "Editor/EditorHierarchyRowPolicy.h"
#include "Editor/EditorSceneWorkspaceState.h"
#include "Editor/EditorSceneVisibilityCommands.h"
#include "Editor/EditorStyle.h"

#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>

namespace PulseForgeEditor
{
	namespace
	{
		std::string FitHierarchyEntityName(std::string_view Name, float AvailableWidth)
		{
			constexpr std::string_view Ellipsis = "\xe2\x80\xa6";
			constexpr size_t MaximumLabelBytes = 480;
			size_t PrefixLength = Utf8SafePrefixLength(Name, MaximumLabelBytes);
			while (PrefixLength > 0)
			{
				const std::string_view Prefix = Name.substr(0, PrefixLength);
				const float Width = ImGui::CalcTextSize(Prefix.data(), Prefix.data() + Prefix.size()).x +
									ImGui::CalcTextSize(Ellipsis.data(), Ellipsis.data() + Ellipsis.size()).x;
				if (Width <= AvailableWidth)
				{
					std::string Result(Prefix);
					Result.append(Ellipsis);
					return Result;
				}
				PrefixLength = Utf8SafePrefixLength(Name, PrefixLength - 1);
			}

			return ImGui::CalcTextSize(Ellipsis.data(), Ellipsis.data() + Ellipsis.size()).x <= AvailableWidth ? std::string(Ellipsis)
																											   : std::string{};
		}

	} // namespace

	EditorIcon GetEditorEntityIcon(const PulseForge::Entity& Entity)
	{
		EditorEntityIconComponents Components;
		const auto Camera = Entity.GetCamera();
		Components.HasCamera = Camera && Camera->has_value();

		const auto Mesh = Entity.GetMeshRenderer();
		Components.HasMesh = Mesh && Mesh->has_value();
		if (Components.HasCamera || Components.HasMesh) return GetEditorEntityIcon(Components);

		const auto SpotLight = Entity.GetSpotLight();
		Components.HasSpotLight = SpotLight && SpotLight->has_value();
		if (Components.HasSpotLight) return GetEditorEntityIcon(Components);

		const auto PointLight = Entity.GetPointLight();
		Components.HasPointLight = PointLight && PointLight->has_value();
		if (Components.HasPointLight) return GetEditorEntityIcon(Components);

		const auto DirectionalLight = Entity.GetDirectionalLight();
		Components.HasDirectionalLight = DirectionalLight && DirectionalLight->has_value();
		if (Components.HasDirectionalLight) return GetEditorEntityIcon(Components);

		const auto EnvironmentLight = Entity.GetEnvironmentLight();
		Components.HasEnvironmentLight = EnvironmentLight && EnvironmentLight->has_value();
		if (Components.HasEnvironmentLight) return GetEditorEntityIcon(Components);

		const auto AudioSource = Entity.GetAudioSource();
		Components.HasAudioSource = AudioSource && AudioSource->has_value();
		if (Components.HasAudioSource) return GetEditorEntityIcon(Components);

		const auto AudioListener = Entity.GetAudioListener();
		Components.HasAudioListener = AudioListener && AudioListener->has_value();
		return GetEditorEntityIcon(Components);
	}

	void EditorHierarchyPanel::DrawContents(
		PulseForge::Scene* SceneValue, EditorSceneWorkspaceState& Workspace, EditorStyle& Style, EditorHierarchyPanelActionSink& Actions)
	{
		Workspace.BeginHierarchyFrame();
		if (Style.BeginToolbar("HierarchyToolbar", 44.0f))
		{
			ImGui::BeginDisabled(SceneValue == nullptr);
			if (Style.IconButton(EditorIcon::Add, "Create", "CreateEntity", "Create a new entity", false, 38.0f, 34.0f))
				Actions.HandleHierarchyPanelAction({ EditorHierarchyPanelActionKind::CreateEntity });
			ImGui::SameLine();
			ImGui::BeginDisabled(Workspace.Selection().GetEntities().empty());
			if (Style.IconButton(EditorIcon::Duplicate, "Duplicate", "DuplicateEntity", "Duplicate selected entity", false, 38.0f, 34.0f))
				Actions.HandleHierarchyPanelAction({ EditorHierarchyPanelActionKind::DuplicateSelection });
			ImGui::SameLine();
			if (Style.IconButton(EditorIcon::Delete, "Delete", "DeleteEntity", "Delete selected entity", true, 34.0f, 32.0f))
				Actions.HandleHierarchyPanelAction({ EditorHierarchyPanelActionKind::DeleteSelection });
			ImGui::SameLine();
			if (Style.IconButton(EditorIcon::Prefab, "Prefab", "CreatePrefab", "Create a prefab from the selection", false, 38.0f, 34.0f))
				Actions.HandleHierarchyPanelAction({ EditorHierarchyPanelActionKind::CreatePrefab });
			ImGui::EndDisabled();
			ImGui::EndDisabled();
		}
		Style.EndToolbar();

		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 7.0f));
		if (m_FocusSearchNextFrame)
		{
			ImGui::SetKeyboardFocusHere();
			m_FocusSearchNextFrame = false;
		}
		ImGui::InputTextWithHint("##HierarchySearch", "Search hierarchy...", &m_Search);
		ImGui::PopStyleVar();
		ImGui::Separator();

		if (!SceneValue) Style.EmptyState(EditorIcon::Scene, "No scene is open.");
		else if (SceneValue->GetEntityCount() == 0) Style.EmptyState(EditorIcon::Entity, "This scene has no entities.");
		else
		{
			std::optional<EditorHierarchyPanelAction> DeferredAction;
			const std::vector<PulseForge::Entity> Entities = SceneValue->GetEntities();
			std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash> VisibleEntities = GetVisibleHierarchyEntities(*SceneValue, m_Search);
			const auto* Visible = m_Search.empty() ? nullptr : &VisibleEntities;
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(7.0f, 6.0f));
			for (const PulseForge::Entity& Entity : Entities)
			{
				const auto Parent = Entity.GetParent();
				if (Parent && !Parent->has_value() && (!Visible || Visible->contains(Entity.GetUUID())))
					DrawEntityTree(Entity, Workspace, Style, Actions, DeferredAction, Visible);
			}
			ImGui::PopStyleVar();
			if (DeferredAction) Actions.HandleHierarchyPanelAction(*DeferredAction);
			if (Visible && Visible->empty()) Style.EmptyState(EditorIcon::Search, "No entities match this search.");
		}
		Workspace.CompleteHierarchyFrame();
	}

	void EditorHierarchyPanel::DrawEntityTree(const PulseForge::Entity& Entity, EditorSceneWorkspaceState& Workspace, EditorStyle& Style,
		EditorHierarchyPanelActionSink& Actions, std::optional<EditorHierarchyPanelAction>& DeferredAction,
		const std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash>* VisibleEntities)
	{
		const auto Tag = Entity.GetTag();
		if (!Tag) return;

		const auto Parent = Entity.GetParent();
		if (!Parent)
		{
			Actions.HandleHierarchyPanelAction({ .Kind = EditorHierarchyPanelActionKind::ReportError,
				.Message = "Could not inspect an entity's hierarchy: " + Parent.error().Message });
			return;
		}
		const auto Children = Entity.GetChildren();
		const EditorIcon Icon = GetEditorEntityIcon(Entity);
		const std::string Identifier = Entity.GetUUID().ToString();
		const bool IsSelected = Workspace.Selection().Contains(Entity.GetUUID());
		Workspace.RecordVisibleHierarchyEntity(Entity.GetUUID());
		ImGui::PushID(Identifier.c_str());
		ImGuiTreeNodeFlags Flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanLabelWidth;
		if (!Children || Children->empty()) Flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
		Flags |= ImGuiTreeNodeFlags_FramePadding;

		if (VisibleEntities && Children && !Children->empty()) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
		const bool IsEffectivelyVisible = EditorSceneVisibilityCommands::IsEntityEffectivelyVisible(Entity, Workspace.Visibility());
		ImGui::PushStyleVar(ImGuiStyleVar_Alpha, IsEffectivelyVisible ? 1.0f : 0.48f);
		const float RowStartX = ImGui::GetCursorPosX();
		const float ContentRightX = ImGui::GetWindowContentRegionMax().x;
		const float LabelStartX = RowStartX + ImGui::GetTreeNodeToLabelSpacing() + ImGui::GetFontSize() + ImGui::CalcTextSize(" ").x;
		const ImGuiStyle& ImGuiStyleValue = ImGui::GetStyle();
		const auto InitialRowLayout =
			ComputeEditorHierarchyRowLayout(RowStartX, ContentRightX, LabelStartX, RowStartX, RowStartX, ImGuiStyleValue.FramePadding.x);
		const float MaximumNameWidth = InitialRowLayout.MaximumNameWidth;
		std::string TruncatedNameStorage;
		std::string_view DisplayName = Tag->Name;
		if (Tag->Name.size() > 480 || ImGui::CalcTextSize(Tag->Name.data(), Tag->Name.data() + Tag->Name.size()).x > MaximumNameWidth)
		{
			TruncatedNameStorage = FitHierarchyEntityName(Tag->Name, MaximumNameWidth);
			DisplayName = TruncatedNameStorage;
		}
		const bool NameTruncated = !TruncatedNameStorage.empty() || DisplayName.size() != Tag->Name.size();
		const ImVec2 RowScreenMinimum = ImGui::GetCursorScreenPos();
		const float DisplayNameWidth = ImGui::CalcTextSize(DisplayName.data(), DisplayName.data() + DisplayName.size()).x;
		const float ExpectedTreeItemHitRightX = LabelStartX + DisplayNameWidth + ImGuiStyleValue.FramePadding.x;
		const auto BackgroundRowLayout = ComputeEditorHierarchyRowLayout(
			RowStartX, ContentRightX, LabelStartX, ExpectedTreeItemHitRightX, RowStartX, ImGuiStyleValue.FramePadding.x);
		const ImVec2 RowScreenMaximum(
			RowScreenMinimum.x + BackgroundRowLayout.SelectionBackgroundWidth, RowScreenMinimum.y + EditorHierarchyEyeButtonHeight);
		const bool SelectionRegionHovered = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(RowScreenMinimum, RowScreenMaximum);
		if (IsSelected || SelectionRegionHovered)
		{
			const ImGuiCol Background = IsSelected ? ImGuiCol_Header : ImGuiCol_HeaderHovered;
			ImGui::GetWindowDrawList()->AddRectFilled(
				RowScreenMinimum, RowScreenMaximum, ImGui::GetColorU32(ImGui::GetStyleColorVec4(Background)));
		}
		const float RowFramePaddingY = (std::max)(0.0f, (EditorHierarchyEyeButtonHeight - ImGui::GetFontSize()) * 0.5f);
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGuiStyleValue.FramePadding.x, RowFramePaddingY));
		ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
		ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
		ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
		const bool IsOpen = Style.TreeNode(Icon, DisplayName, "Entity", Flags);
		ImGui::PopStyleColor(3);
		ImGui::PopStyleVar();
		const bool TreeItemClicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
		const bool TreeItemRightClicked = ImGui::IsItemClicked(ImGuiMouseButton_Right);
		const bool TreeItemHovered = ImGui::IsItemHovered();
		const ImVec2 TreeItemRectMin = ImGui::GetItemRectMin();
		const ImVec2 TreeItemRectMax = ImGui::GetItemRectMax();
		const float TreeItemHitRightX = TreeItemRectMax.x - ImGui::GetWindowPos().x;
		if (TreeItemRightClicked) ImGui::OpenPopup("Entity Actions");

		ImGui::SameLine(0.0f, 0.0f);
		const auto RowLayout = ComputeEditorHierarchyRowLayout(
			RowStartX, ContentRightX, LabelStartX, TreeItemHitRightX, ImGui::GetCursorPosX(), ImGuiStyleValue.FramePadding.x);
		bool GutterClicked = false;
		bool GutterRightClicked = false;
		bool GutterHovered = false;
		if (RowLayout.SelectionGutterWidth > 0.0f)
		{
			ImGui::SetCursorScreenPos(ImVec2(ImGui::GetWindowPos().x + RowLayout.SelectionGutterStartX, TreeItemRectMin.y));
			(void)ImGui::InvisibleButton("##EntitySelectionGutter", ImVec2(RowLayout.SelectionGutterWidth, EditorHierarchyEyeButtonHeight),
				ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_NoNavFocus);
			GutterClicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
			GutterRightClicked = ImGui::IsItemClicked(ImGuiMouseButton_Right);
			GutterHovered = ImGui::IsItemHovered();
		}
		const bool RowClicked = TreeItemClicked || GutterClicked;
		if (NameTruncated && (TreeItemHovered || GutterHovered) &&
			ImGui::GetIO().MousePos.x < ImGui::GetWindowPos().x + RowLayout.EyeStartX)
			ImGui::SetTooltip("%s", Tag->Name.c_str());
		if (TreeItemRightClicked || GutterRightClicked) ImGui::OpenPopup("Entity Actions");

		ImGui::SameLine(RowLayout.EyeStartX);
		const bool LocallyVisible = Workspace.Visibility().IsLocallyVisible(Entity.GetUUID());
		if (Style.IconButton(LocallyVisible ? EditorIcon::Eye : EditorIcon::EyeSlash, LocallyVisible ? "Visible" : "Hidden",
				"EntityVisibility", LocallyVisible ? "Hide this entity in the editor viewport" : "Show this entity in the editor viewport",
				false, EditorHierarchyEyeButtonWidth, EditorHierarchyEyeButtonHeight))
			Actions.HandleHierarchyPanelAction({ .Kind = EditorHierarchyPanelActionKind::ToggleVisibility, .Entity = Entity.GetUUID() });
		ImGui::PopStyleVar();
		if (RowClicked)
		{
			const ImGuiIO& IO = ImGui::GetIO();
			if (IO.KeyShift) (void)Workspace.Selection().SelectRange(Workspace.VisibleHierarchyOrder(), Entity.GetUUID(), IO.KeyCtrl);
			else if (IO.KeyCtrl) Workspace.Selection().Toggle(Entity.GetUUID());
			else Workspace.Selection().SelectSingle(Entity.GetUUID());
		}
		if (ImGui::BeginPopup("Entity Actions"))
		{
			if (!Workspace.Selection().Contains(Entity.GetUUID())) Workspace.Selection().SelectSingle(Entity.GetUUID());
			const PulseForge::UUID ChildIdentifier = Entity.GetUUID();
			const bool HasParent = Parent->has_value();
			if (ImGui::MenuItem("Make Root", nullptr, false, HasParent))
				DeferredAction = EditorHierarchyPanelAction{ .Kind = EditorHierarchyPanelActionKind::MakeRoot, .Entity = ChildIdentifier };
			if (ImGui::MenuItem(LocallyVisible ? "Hide in Viewport" : "Show in Viewport"))
				DeferredAction =
					EditorHierarchyPanelAction{ .Kind = EditorHierarchyPanelActionKind::ToggleVisibility, .Entity = ChildIdentifier };

			const std::optional<PulseForge::UUID> ParentIdentifier = Workspace.Selection().GetPrimary();
			const bool CanParentUnderSelection = ParentIdentifier && *ParentIdentifier != ChildIdentifier;
			if (ImGui::MenuItem("Parent Under Selected", nullptr, false, CanParentUnderSelection))
				DeferredAction = EditorHierarchyPanelAction{
					.Kind = EditorHierarchyPanelActionKind::ParentUnderSelection, .Entity = ChildIdentifier, .Parent = ParentIdentifier
				};
			ImGui::EndPopup();
		}
		if (IsOpen && Children && !Children->empty())
		{
			for (const PulseForge::Entity& Child : *Children)
				if (!VisibleEntities || VisibleEntities->contains(Child.GetUUID()))
					DrawEntityTree(Child, Workspace, Style, Actions, DeferredAction, VisibleEntities);
			ImGui::TreePop();
		}
		ImGui::PopID();
	}
} // namespace PulseForgeEditor
