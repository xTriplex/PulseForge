#pragma once

#include <cstdint>
#include <limits>

namespace PulseForgeEditor
{
	inline constexpr uint32_t NoInspectorHistoryItem = (std::numeric_limits<uint32_t>::max)();

	[[nodiscard]] constexpr bool ShouldFinishSceneEditForInspector(
		bool TransactionActive,
		uint32_t InspectorHistoryItem,
		uint32_t ActiveItem) noexcept
	{
		return TransactionActive && InspectorHistoryItem != NoInspectorHistoryItem &&
			InspectorHistoryItem != ActiveItem;
	}

	struct EditorShortcutContext
	{
		bool SceneAvailable = false;
		bool RuntimeActive = false;
		bool CameraNavigationActive = false;
		bool GizmoInteractionActive = false;
		bool TextInputActive = false;
		bool ImGuiItemActive = false;
		bool PopupOpen = false;
		bool MenuNavigationActive = false;
		bool WindowNavigationActive = false;
	};

	[[nodiscard]] constexpr bool CanRouteEditorShortcut(const EditorShortcutContext& Context) noexcept
	{
		return Context.SceneAvailable && !Context.RuntimeActive && !Context.CameraNavigationActive &&
			!Context.GizmoInteractionActive && !Context.TextInputActive && !Context.ImGuiItemActive &&
			!Context.PopupOpen && !Context.MenuNavigationActive && !Context.WindowNavigationActive;
	}

	struct ViewportGizmoPointerState
	{
		bool InteractionActive = false;
		bool LeftButtonDown = false;
		bool LeftButtonReleased = false;
	};

	enum class ViewportGizmoPointerOutcome
	{
		Idle,
		Continue,
		Complete,
		Cancel
	};

	[[nodiscard]] constexpr ViewportGizmoPointerOutcome GetViewportGizmoPointerOutcome(
		const ViewportGizmoPointerState& State,
		bool EscapePressed) noexcept
	{
		if (!State.InteractionActive)
			return ViewportGizmoPointerOutcome::Idle;
		if (EscapePressed)
			return ViewportGizmoPointerOutcome::Cancel;
		return State.LeftButtonReleased || !State.LeftButtonDown
			? ViewportGizmoPointerOutcome::Complete
			: ViewportGizmoPointerOutcome::Continue;
	}
}
