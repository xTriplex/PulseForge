#pragma once

namespace PulseForgeEditor
{
	[[nodiscard]] constexpr bool ShouldDetachDocumentTab(
		bool MouseReleased,
		bool HasActiveTabDrag,
		bool TransferQueued,
		float DragDistanceSquared,
		float DragThreshold) noexcept
	{
		// Only a registered document strip is a valid docking destination. Releasing over
		// any other editor viewport is therefore a tear-off, just like releasing on desktop.
		// The explicit threshold also supports outside releases where ImGui clears its payload.
		return MouseReleased && HasActiveTabDrag && !TransferQueued &&
			DragDistanceSquared >= DragThreshold * DragThreshold;
	}
}
