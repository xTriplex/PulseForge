#pragma once

#include "Editor/EditorInteractionPolicy.h"
#include "Editor/EditorSceneHistoryState.h"

#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace PulseForgeEditor
{
	class EditorSceneEditController final
	{
	public:
		using RestoreCallback = std::function<std::expected<void, std::string>(std::string_view)>;

		[[nodiscard]] bool IsActive() const noexcept { return m_History.IsActive(); }
		[[nodiscard]] bool IsDirty() const noexcept { return m_History.IsDirty(); }
		[[nodiscard]] bool CanUndo() const noexcept { return m_History.CanUndo(); }
		[[nodiscard]] bool CanRedo() const noexcept { return m_History.CanRedo(); }
		[[nodiscard]] uint32_t& InspectorHistoryItem() noexcept { return m_History.InspectorHistoryItem(); }
		[[nodiscard]] const uint32_t& InspectorHistoryItem() const noexcept { return m_History.InspectorHistoryItem(); }
		[[nodiscard]] std::optional<EditorCommandHistory::CommandView> GetUndoCommand() const noexcept
		{
			return m_History.GetUndoCommand();
		}
		[[nodiscard]] std::optional<EditorCommandHistory::CommandView> GetRedoCommand() const noexcept
		{
			return m_History.GetRedoCommand();
		}
		void MarkDirty() noexcept { m_History.MarkDirty(); }

		[[nodiscard]] std::expected<std::string, std::string> Capture(
			const PulseForge::Scene& SceneValue,
			const EditorSceneWorkspaceState& Workspace) const;
		[[nodiscard]] std::expected<void, std::string> Restore(
			std::string_view State,
			PulseForge::Scene& SceneValue,
			EditorSceneWorkspaceState& Workspace) const;
		void Reset(const PulseForge::Scene* SceneValue);
		void UpdateDirty(const PulseForge::Scene* SceneValue);
		void MarkSaved(const PulseForge::Scene* SceneValue);
		[[nodiscard]] std::expected<void, std::string> Begin(
			PulseForge::Scene* SceneValue,
			EditorSceneWorkspaceState& Workspace,
			bool RuntimeActive,
			std::string Description,
			const RestoreCallback& RestoreState);
		[[nodiscard]] std::expected<void, std::string> Finish(
			PulseForge::Scene* SceneValue,
			EditorSceneWorkspaceState& Workspace,
			const RestoreCallback& RestoreState);
		[[nodiscard]] std::expected<void, std::string> Cancel(
			PulseForge::Scene* SceneValue,
			EditorSceneWorkspaceState& Workspace,
			const RestoreCallback& RestoreState);
		[[nodiscard]] std::expected<bool, std::string> Undo(
			PulseForge::Scene* SceneValue,
			EditorSceneWorkspaceState& Workspace,
			bool RuntimeActive,
			const RestoreCallback& RestoreState);
		[[nodiscard]] std::expected<bool, std::string> Redo(
			PulseForge::Scene* SceneValue,
			EditorSceneWorkspaceState& Workspace,
			bool RuntimeActive,
			const RestoreCallback& RestoreState);

		template<typename Action>
		[[nodiscard]] std::expected<void, std::string> Run(
			PulseForge::Scene* SceneValue,
			EditorSceneWorkspaceState& Workspace,
			bool RuntimeActive,
			uint32_t ActiveInspectorItem,
			const std::function<uint32_t()>& GetActiveInspectorItem,
			std::string Description,
			Action&& Operation,
			const RestoreCallback& RestoreState)
		{
			if (!SceneValue || RuntimeActive)
				return std::unexpected("Scene editing is unavailable while the runtime scene is active");

			if (m_History.IsActive() && m_History.InspectorHistoryItem() != ActiveInspectorItem)
				if (auto Completed = Finish(SceneValue, Workspace, RestoreState); !Completed)
					return Completed;

			if (!m_History.IsActive())
			{
				auto Before = Capture(*SceneValue, Workspace);
				if (!Before)
					return std::unexpected("Could not begin the scene edit: " + Before.error());
				if (auto Begun = m_History.Begin(std::move(Description), std::move(*Before)); !Begun)
					return std::unexpected(Begun.error());
				m_History.InspectorHistoryItem() = ActiveInspectorItem;
			}

			auto Result = std::forward<Action>(Operation)();
			if (!Result)
			{
				const std::string OperationError = Result.error().Message;
				auto Rollback = Cancel(SceneValue, Workspace, RestoreState);
				if (!Rollback)
					return std::unexpected(OperationError + "; scene rollback failed: " + Rollback.error());
				return std::unexpected(OperationError);
			}

			if (ActiveInspectorItem == 0 || !GetActiveInspectorItem ||
				GetActiveInspectorItem() != ActiveInspectorItem)
			{
				if (auto Completed = Finish(SceneValue, Workspace, RestoreState); !Completed)
					return Completed;
			}
			else
				m_History.MarkDirty();
			return {};
		}

	private:
		EditorSceneHistoryState m_History;
	};
}
