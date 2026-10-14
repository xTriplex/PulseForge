#pragma once

#include "Editor/EditorIcons.h"
#include "Editor/EditorViewportToolState.h"

#include <optional>
#include <string_view>

namespace PulseForgeEditor
{
	class EditorStyle;

	struct EditorViewportToolbarActions
	{
		std::optional<ViewportTool> SelectedTool;
		bool ToggleCoordinateSpace = false;
	};

	class EditorViewportToolbar final
	{
	public:
		[[nodiscard]] EditorViewportToolbarActions Draw(
			EditorStyle& Style,
			EditorViewportToolState& Tools,
			bool CanEditViewport) const;

	private:
		static void DrawToolButton(
			EditorStyle& Style,
			const EditorViewportToolState& Tools,
			EditorViewportToolbarActions& Actions,
			ViewportTool Tool,
			EditorIcon Icon,
			std::string_view Label,
			std::string_view StableID,
			std::string_view Tooltip,
			bool CanEditViewport);
		[[nodiscard]] static const char* GetToolName(ViewportTool Tool) noexcept;
		static void DrawCoordinateSpaceControl(
			const EditorViewportToolState& Tools,
			EditorViewportToolbarActions& Actions);
		static void DrawSeparator(EditorStyle& Style);
		static void DrawMoreButton(EditorStyle& Style);
		static void DrawToolMenuItems(
			EditorStyle& Style,
			const EditorViewportToolState& Tools,
			EditorViewportToolbarActions& Actions,
			bool CanEditViewport);
		static void DrawOptionsPopup(
			EditorStyle& Style,
			EditorViewportToolState& Tools,
			EditorViewportToolbarActions& Actions,
			bool IncludeCoordinateSpace,
			bool IncludeSnapToggle);
		static void DrawSnapToggle(EditorViewportToolValues& Values);
		static void DrawSnapIncrementControl(
			EditorViewportToolState& Tools,
			bool ShowLabel);
		static void DrawExposureControl(EditorStyle& Style, EditorViewportToolValues& Values, bool ShowLabel);
	};
}
