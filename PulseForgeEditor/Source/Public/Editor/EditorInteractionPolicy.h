#pragma once

namespace PulseForgeEditor
{
	struct EditorHistoryShortcutContext
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

	[[nodiscard]] constexpr bool CanRouteEditorHistoryShortcut(
		const EditorHistoryShortcutContext& Context) noexcept
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
