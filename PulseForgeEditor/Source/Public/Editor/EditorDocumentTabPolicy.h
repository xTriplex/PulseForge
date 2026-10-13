#pragma once

namespace PulseForgeEditor
{
	[[nodiscard]] constexpr bool ShouldDetachDocumentTab(
		bool MouseReleased,
		bool HasDocumentPayload,
		bool TransferQueued,
		bool PointerOverEditorViewport) noexcept
	{
		return MouseReleased && HasDocumentPayload && !TransferQueued && !PointerOverEditorViewport;
	}
}
