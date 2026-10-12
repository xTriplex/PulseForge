#pragma once

#include <algorithm>

namespace PulseForgeEditor
{
	inline constexpr float EditorHierarchyEyeButtonWidth = 25.0f;
	inline constexpr float EditorHierarchyEyeButtonHeight = 24.0f;
	inline constexpr float EditorHierarchyEyeRightInset = 5.0f;
	inline constexpr float EditorHierarchyHitRegionGap = 4.0f;
	inline constexpr float EditorHierarchyHitRegionSafetyInset = 4.0f;

	struct EditorHierarchyRowLayout
	{
		float EyeStartX;
		float MaximumNameWidth;
		float SelectionGutterStartX;
		float SelectionGutterWidth;
	};

	[[nodiscard]] constexpr EditorHierarchyRowLayout ComputeEditorHierarchyRowLayout(
		float RowStartX,
		float ContentRightX,
		float NameStartX,
		float TreeItemHitRightX,
		float ItemCursorX,
		float FramePaddingX) noexcept
	{
		const float DefaultEyeStartX = (std::max)(
			RowStartX,
			ContentRightX - EditorHierarchyEyeButtonWidth - EditorHierarchyEyeRightInset);
		const float TreeItemTrailingHitWidth = FramePaddingX;
		const float MaximumNameWidth = (std::max)(0.0f,
			DefaultEyeStartX - NameStartX - EditorHierarchyHitRegionGap - EditorHierarchyHitRegionSafetyInset -
			TreeItemTrailingHitWidth);

		const float EyeStartX = (std::max)(DefaultEyeStartX, TreeItemHitRightX + EditorHierarchyHitRegionGap);
		const float SelectionGutterStartX = (std::max)(TreeItemHitRightX, ItemCursorX);
		const float SelectionGutterEndX = (std::max)(SelectionGutterStartX, EyeStartX - EditorHierarchyHitRegionGap);
		return {
			EyeStartX,
			MaximumNameWidth,
			SelectionGutterStartX,
			SelectionGutterEndX - SelectionGutterStartX
		};
	}
}
