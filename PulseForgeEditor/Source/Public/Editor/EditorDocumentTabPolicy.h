#pragma once

namespace PulseForgeEditor
{
	[[nodiscard]] constexpr bool ShouldDetachDocumentTab(
		bool MouseReleased,
		bool HasActiveTabDrag,
		bool TransferQueued,
		bool PointerOverEditorViewport) noexcept
	{
		return MouseReleased && HasActiveTabDrag && !TransferQueued && !PointerOverEditorViewport;
	}
}
