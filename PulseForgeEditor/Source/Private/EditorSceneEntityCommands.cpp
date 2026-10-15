#include "Editor/EditorSceneEntityCommands.h"

#include "Editor/EditorSceneHierarchyOperations.h"
#include "Editor/EditorSceneWorkspaceState.h"
#include "Scene/SceneSerializer.h"

#include <utility>

namespace PulseForgeEditor
{
	void EditorSceneEntityCommands::CreateEntity(
		PulseForge::Scene* SceneValue,
		EditorSceneEntityCommandActions& Actions) const
	{
		if (!SceneValue)
			return;

		const auto Recorded = Actions.RunEditorSceneEdit(
			"Create Entity",
			[SceneValue, &Actions]() -> std::expected<void, PulseForge::SceneError>
			{
				auto Created = SceneValue->CreateEntity();
				if (!Created)
					return std::unexpected(Created.error());
				Actions.SelectSingleEditorEntity(Created->GetUUID());
				return {};
			});
		if (!Recorded)
			Actions.ReportEditorSceneCommandError("Entity creation failed: " + Recorded.error());
	}

	void EditorSceneEntityCommands::DuplicateSelectedEntities(
		PulseForge::Scene* SceneValue,
		EditorSceneWorkspaceState& Workspace,
		EditorSceneEntityCommandActions& Actions) const
	{
		if (!SceneValue || Workspace.Selection().GetEntities().empty())
			return;

		auto Roots = EditorSceneHierarchyOperations::GetTopLevelSelection(
			*SceneValue, Workspace.Selection().GetEntities());
		if (!Roots)
		{
			Actions.ReportEditorSceneCommandError("Could not duplicate the selection: " + Roots.error());
			return;
		}

		std::vector<PulseForge::UUID> DuplicatedRoots;
		const auto Recorded = Actions.RunEditorSceneEdit(
			"Duplicate Entities",
			[SceneValue, Roots = std::move(*Roots), &DuplicatedRoots, &Actions]()
				-> std::expected<void, PulseForge::SceneError>
			{
				auto Result = EditorSceneHierarchyOperations::DuplicateSubtrees(*SceneValue, Roots);
				if (!Result)
					return std::unexpected(Result.error());
				DuplicatedRoots = std::move(*Result);
				Actions.SetEditorSceneSelection(DuplicatedRoots);
				return {};
			});
		if (!Recorded)
			Actions.ReportEditorSceneCommandError("Entity duplication failed: " + Recorded.error());
		else
			Actions.SetEditorSceneCommandStatus(
				"Duplicated " + std::to_string(DuplicatedRoots.size()) + " selected hierarchy root(s).");
	}

	void EditorSceneEntityCommands::DeleteSelectedEntities(
		PulseForge::Scene* SceneValue,
		EditorSceneWorkspaceState& Workspace,
		EditorSceneEntityCommandActions& Actions) const
	{
		if (!SceneValue || Workspace.Selection().GetEntities().empty())
			return;

		auto Roots = EditorSceneHierarchyOperations::GetTopLevelSelection(
			*SceneValue, Workspace.Selection().GetEntities());
		if (!Roots)
		{
			Actions.ReportEditorSceneCommandError("Could not delete the selection: " + Roots.error());
			return;
		}
		auto Subtree = EditorSceneHierarchyOperations::CollectSubtrees(*Roots);
		if (!Subtree)
		{
			Actions.ReportEditorSceneCommandError("Could not delete the selection: " + Subtree.error());
			return;
		}

		const size_t DeleteCount = Subtree->size();
		const auto Recorded = Actions.RunEditorSceneEdit(
			"Delete Entities",
			[SceneValue, &Workspace, &Actions, Subtree = std::move(*Subtree)]()
				-> std::expected<void, PulseForge::SceneError>
			{
				for (auto Entity = Subtree.rbegin(); Entity != Subtree.rend(); ++Entity)
				{
					const auto Target = SceneValue->FindEntity(Entity->GetUUID());
					if (!Target)
						continue;
					Workspace.Visibility().Remove(Entity->GetUUID());
					if (auto Deleted = SceneValue->DestroyEntity(*Target); !Deleted)
						return std::unexpected(Deleted.error());
				}
				Actions.SelectSingleEditorEntity(std::nullopt);
				return {};
			});
		if (!Recorded)
			Actions.ReportEditorSceneCommandError("Entity deletion failed: " + Recorded.error());
		else
			Actions.SetEditorSceneCommandStatus("Deleted " + std::to_string(DeleteCount) + " entities.");
	}

	bool EditorSceneEntityCommands::CopySelectedEntities(
		PulseForge::Scene* SceneValue,
		const PulseForge::UUID& ProjectID,
		std::optional<PulseForge::AssetID> SceneAsset,
		EditorSceneWorkspaceState& Workspace,
		EditorSceneCommandActions& Actions)
	{
		if (!SceneValue || Workspace.Selection().GetEntities().empty())
			return false;

		auto Roots = EditorSceneHierarchyOperations::GetTopLevelSelection(
			*SceneValue, Workspace.Selection().GetEntities());
		if (!Roots)
		{
			Actions.ReportEditorSceneCommandError("Could not copy the selection: " + Roots.error());
			return false;
		}
		auto Subtree = EditorSceneHierarchyOperations::CollectSubtrees(*Roots);
		if (!Subtree)
		{
			Actions.ReportEditorSceneCommandError("Could not copy the selection: " + Subtree.error());
			return false;
		}

		auto Serialized = PulseForge::SceneSerializer::Serialize(*SceneValue);
		if (!Serialized)
		{
			Actions.ReportEditorSceneCommandError(
				"Could not serialize the selection for the clipboard: " + Serialized.error().Message);
			return false;
		}

		std::vector<PulseForge::UUID> RootIdentifiers;
		RootIdentifiers.reserve(Roots->size());
		for (const PulseForge::Entity& Root : *Roots)
			RootIdentifiers.push_back(Root.GetUUID());
		std::vector<PulseForge::UUID> IncludedIdentifiers;
		IncludedIdentifiers.reserve(Subtree->size());
		for (const PulseForge::Entity& Entity : *Subtree)
			IncludedIdentifiers.push_back(Entity.GetUUID());

		auto Clipboard = EditorEntityClipboard::Capture(
			ProjectID, SceneAsset, RootIdentifiers, IncludedIdentifiers, *Serialized);
		if (!Clipboard)
		{
			Actions.ReportEditorSceneCommandError("Could not encode the selection for the clipboard: " + Clipboard.error());
			return false;
		}

		m_Clipboard = std::move(*Clipboard);
		Actions.SetEditorSceneCommandStatus("Copied " + std::to_string(Subtree->size()) + " entities.");
		return true;
	}

	void EditorSceneEntityCommands::CutSelectedEntities(
		PulseForge::Scene* SceneValue,
		const PulseForge::UUID& ProjectID,
		std::optional<PulseForge::AssetID> SceneAsset,
		EditorSceneWorkspaceState& Workspace,
		EditorSceneEntityCommandActions& Actions)
	{
		if (Workspace.Selection().GetEntities().empty())
			return;
		if (CopySelectedEntities(SceneValue, ProjectID, SceneAsset, Workspace, Actions))
			DeleteSelectedEntities(SceneValue, Workspace, Actions);
	}

	void EditorSceneEntityCommands::PasteEntities(
		PulseForge::Scene* SceneValue,
		const PulseForge::UUID& ProjectID,
		std::optional<PulseForge::AssetID> SceneAsset,
		EditorSceneWorkspaceState& Workspace,
		bool RuntimeActive,
		const std::function<bool(const PulseForge::UUID&)>& IsAssetRegistered,
		EditorSceneEntityCommandActions& Actions) const
	{
		if (!SceneValue || !m_Clipboard.HasData() || RuntimeActive)
			return;
		if (!m_Clipboard.CanPaste(ProjectID))
		{
			Actions.ReportEditorSceneCommandWarning(
				"The clipboard belongs to another project; copy the entities again in this project.");
			return;
		}

		auto Serialized = PulseForge::SceneSerializer::Serialize(*SceneValue);
		if (!Serialized)
		{
			Actions.ReportEditorSceneCommandError(
				"Could not prepare the active scene for paste: " + Serialized.error().Message);
			return;
		}

		auto Prepared = m_Clipboard.PreparePaste(ProjectID, SceneAsset, *Serialized, IsAssetRegistered);
		if (!Prepared)
		{
			Actions.ReportEditorSceneCommandError("Paste failed: " + Prepared.error());
			return;
		}

		const size_t PastedCount = Prepared->EntityCount;
		const std::string MergedScene = std::move(Prepared->SerializedScene);
		const std::vector<PulseForge::UUID> NewRoots = std::move(Prepared->RootIdentifiers);
		const auto Recorded = Actions.RunEditorSceneEdit(
			"Paste Entities",
			[SceneValue, &Actions, MergedScene, NewRoots]() -> std::expected<void, PulseForge::SceneError>
			{
				auto Result = PulseForge::SceneSerializer::Deserialize(MergedScene, *SceneValue);
				if (!Result)
					return std::unexpected(PulseForge::SceneError{
						PulseForge::SceneErrorCode::InvalidEntity, Result.error().Message });
				Actions.SetEditorSceneSelection(NewRoots);
				return {};
			});
		if (!Recorded)
		{
			Actions.ReportEditorSceneCommandError("Paste failed: " + Recorded.error());
			return;
		}
		Actions.SetEditorSceneCommandStatus("Pasted " + std::to_string(PastedCount) + " entities.");
	}

	void EditorSceneEntityCommands::ReparentEntity(
		PulseForge::Scene* SceneValue,
		const PulseForge::UUID& ChildIdentifier,
		std::optional<PulseForge::UUID> ParentIdentifier,
		EditorSceneCommandActions& Actions) const
	{
		if (!SceneValue)
			return;
		const auto Child = SceneValue->FindEntity(ChildIdentifier);
		if (!Child)
		{
			Actions.ReportEditorSceneCommandError("The hierarchy entity no longer exists.");
			return;
		}
		if (ParentIdentifier && !SceneValue->FindEntity(*ParentIdentifier))
		{
			Actions.ReportEditorSceneCommandError("The selected parent entity no longer exists.");
			return;
		}

		const auto Recorded = Actions.RunEditorSceneEdit(
			"Reparent Entity",
			[SceneValue, Child = *Child, ParentIdentifier]() -> std::expected<void, PulseForge::SceneError>
			{
				if (ParentIdentifier)
				{
					const auto Parent = SceneValue->FindEntity(*ParentIdentifier);
					if (!Parent)
						return std::unexpected(PulseForge::SceneError{
							PulseForge::SceneErrorCode::InvalidEntity,
							"The selected parent entity no longer exists" });
					return Child.SetParent(*Parent);
				}
				return Child.ClearParent();
			});
		if (!Recorded)
			Actions.ReportEditorSceneCommandError("Entity reparenting failed: " + Recorded.error());
	}

	bool EditorSceneEntityCommands::CanPaste(const PulseForge::UUID& ProjectID) const noexcept
	{
		return m_Clipboard.CanPaste(ProjectID);
	}
}
