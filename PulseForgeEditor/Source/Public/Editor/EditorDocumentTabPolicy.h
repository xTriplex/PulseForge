#pragma once

namespace PulseForgeEditor
{
	[[nodiscard]] constexpr bool ShouldDetachDocumentTab(
		bool MouseReleased,
		bool HasActiveTabDrag,
		bool TransferQueued) noexcept
	{
		// Only a registered document strip is a valid docking destination. Releasing over
		// any other editor viewport is therefore a tear-off, just like releasing on desktop.
		return MouseReleased && HasActiveTabDrag && !TransferQueued;
	}
}
