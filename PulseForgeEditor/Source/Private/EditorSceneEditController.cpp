#include "Editor/EditorSceneEditController.h"

namespace PulseForgeEditor
{
	std::expected<std::string, std::string> EditorSceneEditController::Capture(
		const PulseForge::Scene& SceneValue,
		const EditorSceneWorkspaceState& Workspace) const
	{
		return EditorSceneHistoryState::CaptureSnapshot(SceneValue, Workspace);
	}

	std::expected<void, std::string> EditorSceneEditController::Restore(
		std::string_view State,
		PulseForge::Scene& SceneValue,
		EditorSceneWorkspaceState& Workspace) const
	{
		return EditorSceneHistoryState::RestoreSnapshot(State, SceneValue, Workspace);
	}

	void EditorSceneEditController::Reset(const PulseForge::Scene* SceneValue)
	{
		m_History.Reset(SceneValue);
	}

	void EditorSceneEditController::UpdateDirty(const PulseForge::Scene* SceneValue)
	{
		m_History.UpdateDirty(SceneValue);
	}

	void EditorSceneEditController::MarkSaved(const PulseForge::Scene* SceneValue)
	{
		m_History.MarkSaved(SceneValue);
	}

	std::expected<void, std::string> EditorSceneEditController::Begin(
		PulseForge::Scene* SceneValue,
		EditorSceneWorkspaceState& Workspace,
		bool RuntimeActive,
		std::string Description,
		const RestoreCallback& RestoreState)
	{
		if (m_History.IsActive())
			if (auto Completed = Finish(SceneValue, Workspace, RestoreState); !Completed)
				return Completed;
		if (!SceneValue || RuntimeActive)
			return std::unexpected("Scene editing is unavailable while the runtime scene is active");

		auto Before = Capture(*SceneValue, Workspace);
		if (!Before)
			return std::unexpected("Could not begin the scene edit: " + Before.error());
		if (auto Begun = m_History.Begin(std::move(Description), std::move(*Before)); !Begun)
			return std::unexpected(Begun.error());
		m_History.InspectorHistoryItem() = NoInspectorHistoryItem;
		return {};
	}

	std::expected<void, std::string> EditorSceneEditController::Finish(
		PulseForge::Scene* SceneValue,
		EditorSceneWorkspaceState& Workspace,
		const RestoreCallback& RestoreState)
	{
		if (!m_History.IsActive())
		{
			m_History.InspectorHistoryItem() = 0;
			return {};
		}
		if (!SceneValue)
			return std::unexpected("The scene was closed while an edit transaction was active");

		auto After = Capture(*SceneValue, Workspace);
		if (!After)
		{
			const std::string Error = After.error();
			auto Rollback = Cancel(SceneValue, Workspace, RestoreState);
			if (!Rollback)
				return std::unexpected("Could not finish the scene edit transaction: " + Error +
					"; rollback failed: " + Rollback.error());
			return std::unexpected("Could not finish the scene edit transaction: " + Error);
		}

		if (auto Committed = m_History.Commit(std::move(*After)); !Committed)
		{
			const std::string Error = Committed.error();
			auto Rollback = Cancel(SceneValue, Workspace, RestoreState);
			if (!Rollback)
				return std::unexpected("Scene edit could not be stored in history: " + Error +
					"; rollback failed: " + Rollback.error());
			return std::unexpected("Scene edit was rolled back because it could not be stored in history: " + Error);
		}

		m_History.InspectorHistoryItem() = 0;
		UpdateDirty(SceneValue);
		return {};
	}

	std::expected<void, std::string> EditorSceneEditController::Cancel(
		PulseForge::Scene* SceneValue,
		EditorSceneWorkspaceState& Workspace,
		const RestoreCallback& RestoreState)
	{
		std::expected<void, std::string> Result;
		if (m_History.IsActive())
		{
			if (!SceneValue)
				Result = std::unexpected("The scene was closed before the active edit could be restored");
			else
				Result = m_History.Cancel([this, SceneValue, &Workspace, &RestoreState](std::string_view State)
				{
					return RestoreState
						? RestoreState(State)
						: Restore(State, *SceneValue, Workspace);
				});
		}
		m_History.InspectorHistoryItem() = 0;
		UpdateDirty(SceneValue);
		return Result;
	}

	std::expected<bool, std::string> EditorSceneEditController::Undo(
		PulseForge::Scene* SceneValue,
		EditorSceneWorkspaceState& Workspace,
		bool RuntimeActive,
		const RestoreCallback& RestoreState)
	{
		if (!SceneValue || RuntimeActive)
			return false;
		if (auto Completed = Finish(SceneValue, Workspace, RestoreState); !Completed)
			return std::unexpected(Completed.error());
		auto Result = m_History.Undo([this, SceneValue, &Workspace, &RestoreState](std::string_view State)
		{
			return RestoreState ? RestoreState(State) : Restore(State, *SceneValue, Workspace);
		});
		if (!Result)
			return std::unexpected(Result.error());
		UpdateDirty(SceneValue);
		return *Result;
	}

	std::expected<bool, std::string> EditorSceneEditController::Redo(
		PulseForge::Scene* SceneValue,
		EditorSceneWorkspaceState& Workspace,
		bool RuntimeActive,
		const RestoreCallback& RestoreState)
	{
		if (!SceneValue || RuntimeActive)
			return false;
		if (auto Completed = Finish(SceneValue, Workspace, RestoreState); !Completed)
			return std::unexpected(Completed.error());
		auto Result = m_History.Redo([this, SceneValue, &Workspace, &RestoreState](std::string_view State)
		{
			return RestoreState ? RestoreState(State) : Restore(State, *SceneValue, Workspace);
		});
		if (!Result)
			return std::unexpected(Result.error());
		UpdateDirty(SceneValue);
		return *Result;
	}
}
