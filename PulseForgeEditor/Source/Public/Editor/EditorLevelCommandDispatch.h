#pragma once

#include "Editor/EditorDocumentManager.h"
#include "Editor/EditorInteractionPolicy.h"

#include <optional>
#include <utility>

namespace PulseForgeEditor
{
	enum class EditorDocumentShortcutCommand
	{
		Save,
		SaveAs,
		Undo,
		Redo
	};

	enum class EditorLevelCommandDispatchResult
	{
		NotApplicable,
		Suppressed,
		Dispatched
	};

	[[nodiscard]] inline std::optional<EditorDocumentShortcutCommand> ResolveEditorDocumentShortcut(
		bool Control, bool Shift, bool Alt, bool Super, bool SavePressed, bool UndoPressed, bool RedoPressed) noexcept
	{
		if (!Control || Alt || Super) return std::nullopt;
		if (SavePressed) return Shift ? EditorDocumentShortcutCommand::SaveAs : EditorDocumentShortcutCommand::Save;
		if (RedoPressed || (Shift && UndoPressed)) return EditorDocumentShortcutCommand::Redo;
		if (UndoPressed) return EditorDocumentShortcutCommand::Undo;
		return std::nullopt;
	}

	template<typename Callback>
	[[nodiscard]] EditorLevelCommandDispatchResult DispatchEditorLevelCommand(
		const std::optional<EditorDocumentIdentity>& Owner, std::optional<EditorDocumentShortcutCommand> Command,
		const EditorShortcutContext& Context,
		Callback&& Dispatch)
	{
		if (!Owner || Owner->Kind != EditorDocumentKind::Level || !Command)
			return EditorLevelCommandDispatchResult::NotApplicable;
		if (!CanRouteEditorShortcut(Context)) return EditorLevelCommandDispatchResult::Suppressed;
		std::forward<Callback>(Dispatch)(*Command);
		return EditorLevelCommandDispatchResult::Dispatched;
	}
} // namespace PulseForgeEditor
