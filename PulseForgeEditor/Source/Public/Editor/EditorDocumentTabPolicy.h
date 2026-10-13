#pragma once

#include <cstdint>

namespace PulseForgeEditor
{
	enum class DocumentTabReleaseAction : uint8_t
	{
		None,
		TransferToStrip,
		CreateNativeHost
	};

	[[nodiscard]] constexpr bool ShouldCaptureDocumentTabDrag(
		bool TabHovered,
		bool CloseButtonHovered,
		bool LeftButtonPressed) noexcept
	{
		return TabHovered && !CloseButtonHovered && LeftButtonPressed;
	}

	[[nodiscard]] constexpr DocumentTabReleaseAction ResolveDocumentTabReleaseAction(
		bool MouseReleased,
		bool HasActiveTabDrag,
		bool TransferQueued,
		float DragDistanceSquared,
		float DragThreshold,
		bool SourceIsMainHost,
		bool IsAssetDocument,
		bool IsOverRegisteredDocumentStrip) noexcept
	{
		if (!MouseReleased || !HasActiveTabDrag || TransferQueued ||
			!(DragDistanceSquared >= DragThreshold * DragThreshold))
			return DocumentTabReleaseAction::None;
		if (!IsAssetDocument)
			return DocumentTabReleaseAction::None;
		if (IsOverRegisteredDocumentStrip)
			return DocumentTabReleaseAction::TransferToStrip;
		return SourceIsMainHost
			? DocumentTabReleaseAction::CreateNativeHost
			: DocumentTabReleaseAction::None;
	}
}
