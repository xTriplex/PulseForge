#pragma once

#include <cstdint>

namespace PulseForgeEditor
{
	enum class EditorViewportToolbarLayout : uint8_t
	{
		Full,
		Compact,
		Overflow
	};

	inline constexpr float EditorViewportToolbarFullWidth = 590.0f;
	inline constexpr float EditorViewportToolbarCompactWidth = 300.0f;

	[[nodiscard]] constexpr EditorViewportToolbarLayout GetEditorViewportToolbarLayout(float AvailableWidth) noexcept
	{
		if (AvailableWidth >= EditorViewportToolbarFullWidth)
			return EditorViewportToolbarLayout::Full;
		if (AvailableWidth >= EditorViewportToolbarCompactWidth)
			return EditorViewportToolbarLayout::Compact;
		return EditorViewportToolbarLayout::Overflow;
	}
}
