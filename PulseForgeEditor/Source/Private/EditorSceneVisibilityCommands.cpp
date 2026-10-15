#include "Editor/EditorSceneVisibilityCommands.h"

#include "Editor/EditorSceneHierarchyOperations.h"
#include "Editor/EditorSceneWorkspaceState.h"

#include <unordered_set>
#include <utility>

namespace PulseForgeEditor
{
	bool EditorSceneVisibilityCommands::IsEntityEffectivelyVisible(
		const PulseForge::Entity& Entity,
		const EditorVisibility& Visibility)
	{
		std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash> Visited;
		PulseForge::Entity Current = Entity;
		while (Current)
		{
			if (!Visited.insert(Current.GetUUID()).second || !Visibility.IsLocallyVisible(Current.GetUUID()))
				return false;
			const auto Parent = Current.GetParent();
			if (!Parent || !Parent->has_value())
				return Parent.has_value();
			Current = **Parent;
		}
		return false;
	}

	std::vector<PulseForge::UUID> EditorSceneVisibilityCommands::GetEffectivelyHiddenEntities(
		const PulseForge::Scene* SceneValue,
		const EditorVisibility& Visibility)
	{
		std::vector<PulseForge::UUID> Hidden;
		if (!SceneValue)
			return Hidden;
		for (const PulseForge::Entity& Entity : SceneValue->GetEntities())
			if (!IsEntityEffectivelyVisible(Entity, Visibility))
				Hidden.push_back(Entity.GetUUID());
		return Hidden;
	}

	void EditorSceneVisibilityCommands::ToggleEntityVisibility(
		PulseForge::Scene* SceneValue,
		EditorSceneWorkspaceState& Workspace,
		const PulseForge::UUID& Identifier,
		bool RuntimeActive,
		EditorSceneCommandActions& Actions) const
	{
		const auto Entity = SceneValue ? SceneValue->FindEntity(Identifier) : std::nullopt;
		if (!Entity || RuntimeActive)
			return;

		const bool MakeVisible = !Workspace.Visibility().IsLocallyVisible(Identifier);
		const auto Recorded = Actions.RunEditorSceneEdit(
			MakeVisible ? "Show Entity" : "Hide Entity",
			[&Workspace, Identifier, MakeVisible]() -> std::expected<void, PulseForge::SceneError>
			{
				Workspace.Visibility().SetVisible(Identifier, MakeVisible);
				return {};
			});
		if (!Recorded)
			Actions.ReportEditorSceneCommandError("Could not update editor visibility: " + Recorded.error());
	}

	void EditorSceneVisibilityCommands::HideSelectedEntities(
		PulseForge::Scene* SceneValue,
		EditorSceneWorkspaceState& Workspace,
		bool RuntimeActive,
		EditorSceneCommandActions& Actions) const
	{
		const std::vector<PulseForge::UUID> Identifiers = Workspace.Selection().GetEntities();
		if (!SceneValue || RuntimeActive || Identifiers.empty())
			return;

		const auto Recorded = Actions.RunEditorSceneEdit(
			"Hide Selected",
			[&Workspace, Identifiers]() -> std::expected<void, PulseForge::SceneError>
			{
				(void)Workspace.Visibility().Hide(Identifiers);
				return {};
			});
		if (!Recorded)
			Actions.ReportEditorSceneCommandError("Could not hide the selection: " + Recorded.error());
		else
			Actions.SetEditorSceneCommandStatus(
				"Hid " + std::to_string(Identifiers.size()) + " selected entities in the editor viewport.");
	}

	void EditorSceneVisibilityCommands::ShowAllEntities(
		PulseForge::Scene* SceneValue,
		EditorSceneWorkspaceState& Workspace,
		bool RuntimeActive,
		EditorSceneCommandActions& Actions) const
	{
		const size_t RestoredCount = Workspace.Visibility().GetHiddenEntities().size();
		if (!SceneValue || RuntimeActive || RestoredCount == 0)
			return;

		const auto Recorded = Actions.RunEditorSceneEdit(
			"Show All Entities",
			[&Workspace]() -> std::expected<void, PulseForge::SceneError>
			{
				Workspace.Visibility().ShowAll();
				return {};
			});
		if (!Recorded)
			Actions.ReportEditorSceneCommandError("Could not restore editor visibility: " + Recorded.error());
		else
			Actions.SetEditorSceneCommandStatus("Restored visibility for " + std::to_string(RestoredCount) + " hidden entities.");
	}

	void EditorSceneVisibilityCommands::IsolateSelectedEntities(
		PulseForge::Scene* SceneValue,
		EditorSceneWorkspaceState& Workspace,
		bool RuntimeActive,
		EditorSceneCommandActions& Actions) const
	{
		const std::vector<PulseForge::UUID> SelectedIdentifiers = Workspace.Selection().GetEntities();
		if (!SceneValue || RuntimeActive || SelectedIdentifiers.empty())
			return;

		std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash> KeepVisible;
		for (const PulseForge::UUID& Identifier : SelectedIdentifiers)
		{
			const auto Selected = SceneValue->FindEntity(Identifier);
			if (!Selected)
				continue;
			KeepVisible.insert(Identifier);
			PulseForge::Entity ParentWalk = *Selected;
			while (ParentWalk)
			{
				const auto Parent = ParentWalk.GetParent();
				if (!Parent || !Parent->has_value())
					break;
				ParentWalk = **Parent;
				KeepVisible.insert(ParentWalk.GetUUID());
			}
			if (const auto Subtree = EditorSceneHierarchyOperations::CollectSubtrees(
				std::vector<PulseForge::Entity>{ *Selected }))
				for (const PulseForge::Entity& Descendant : *Subtree)
					KeepVisible.insert(Descendant.GetUUID());
		}

		std::vector<PulseForge::UUID> Keep(KeepVisible.begin(), KeepVisible.end());
		std::vector<PulseForge::UUID> All;
		for (const PulseForge::Entity& Entity : SceneValue->GetEntities())
			All.push_back(Entity.GetUUID());

		const auto Recorded = Actions.RunEditorSceneEdit(
			"Isolate Selection",
			[&Workspace, Keep = std::move(Keep), All = std::move(All)]()
				-> std::expected<void, PulseForge::SceneError>
			{
				if (!Workspace.Visibility().Isolate(Keep, All))
					return std::unexpected(PulseForge::SceneError{
						PulseForge::SceneErrorCode::InvalidEntity, "The selection could not be isolated" });
				return {};
			});
		if (!Recorded)
			Actions.ReportEditorSceneCommandError("Could not isolate the selection: " + Recorded.error());
		else
			Actions.SetEditorSceneCommandStatus("Isolated the selection in the editor viewport.");
	}

	void EditorSceneVisibilityCommands::RestoreIsolatedVisibility(
		PulseForge::Scene* SceneValue,
		EditorSceneWorkspaceState& Workspace,
		bool RuntimeActive,
		EditorSceneCommandActions& Actions) const
	{
		if (!SceneValue || RuntimeActive || !Workspace.Visibility().GetIsolationBaseline())
			return;

		const auto Recorded = Actions.RunEditorSceneEdit(
			"Restore Isolated Visibility",
			[&Workspace]() -> std::expected<void, PulseForge::SceneError>
			{
				(void)Workspace.Visibility().RestoreIsolation();
				return {};
			});
		if (!Recorded)
			Actions.ReportEditorSceneCommandError("Could not restore the prior visibility state: " + Recorded.error());
		else
			Actions.SetEditorSceneCommandStatus("Restored visibility before isolation.");
	}
}
