#pragma once

#include "Editor/EditorCommandHistory.h"
#include "Editor/EditorSceneWorkspaceState.h"
#include "Scene/Scene.h"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace PulseForgeEditor
{
	// Couples authored-scene snapshots and saved-state tracking to bounded history.
	// Snapshots include editor selection and visibility so undo restores the working context.
	class EditorSceneHistoryState final
	{
	public:
		[[nodiscard]] static std::expected<std::string, std::string> CaptureSnapshot(
			const PulseForge::Scene& SceneValue,
			const EditorSceneWorkspaceState& Workspace);
		[[nodiscard]] static std::expected<void, std::string> RestoreSnapshot(
			std::string_view State,
			PulseForge::Scene& SceneValue,
			EditorSceneWorkspaceState& Workspace);

		void Reset(const PulseForge::Scene* SceneValue);
		void UpdateDirty(const PulseForge::Scene* SceneValue);
		void MarkSaved(const PulseForge::Scene* SceneValue);
		void MarkDirty() noexcept { m_Dirty = true; }
		[[nodiscard]] bool IsDirty() const noexcept { return m_Dirty; }
		[[nodiscard]] uint32_t& InspectorHistoryItem() noexcept { return m_InspectorHistoryItem; }
		[[nodiscard]] const uint32_t& InspectorHistoryItem() const noexcept { return m_InspectorHistoryItem; }

		void ResetCommands() noexcept { m_Commands.Reset(); }
		[[nodiscard]] std::expected<void, std::string> Begin(std::string Description, std::string StateBefore);
		[[nodiscard]] std::expected<bool, std::string> Commit(std::string StateAfter);
		template<typename Restore>
		[[nodiscard]] std::expected<void, std::string> Cancel(Restore&& RestoreState)
		{
			return m_Commands.Cancel(std::forward<Restore>(RestoreState));
		}
		template<typename Restore>
		[[nodiscard]] std::expected<bool, std::string> Undo(Restore&& RestoreState)
		{
			return m_Commands.Undo(std::forward<Restore>(RestoreState));
		}
		template<typename Restore>
		[[nodiscard]] std::expected<bool, std::string> Redo(Restore&& RestoreState)
		{
			return m_Commands.Redo(std::forward<Restore>(RestoreState));
		}
		void MarkHistorySaved() noexcept { m_Commands.MarkSaved(); }
		[[nodiscard]] bool IsActive() const noexcept { return m_Commands.IsActive(); }
		[[nodiscard]] bool CanUndo() const noexcept { return m_Commands.CanUndo(); }
		[[nodiscard]] bool CanRedo() const noexcept { return m_Commands.CanRedo(); }
		[[nodiscard]] std::optional<EditorCommandHistory::CommandView> GetUndoCommand() const noexcept
		{
			return m_Commands.GetUndoCommand();
		}
		[[nodiscard]] std::optional<EditorCommandHistory::CommandView> GetRedoCommand() const noexcept
		{
			return m_Commands.GetRedoCommand();
		}

	private:
		EditorCommandHistory m_Commands;
		std::optional<std::string> m_SavedSceneSnapshot;
		uint32_t m_InspectorHistoryItem = 0;
		bool m_Dirty = false;
	};
}
