#pragma once

#include <cmath>
#include <cstdint>
#include <optional>

namespace PulseForgeEditor
{
	struct DocumentTabPointerPosition
	{
		float X = 0.0f;
		float Y = 0.0f;
	};

	[[nodiscard]] inline bool IsUsableDocumentTabPointerPosition(float X, float Y) noexcept
	{
		return std::isfinite(X) && std::isfinite(Y) && std::abs(X) <= 100000.0f && std::abs(Y) <= 100000.0f;
	}

	[[nodiscard]] inline std::optional<DocumentTabPointerPosition> ResolveDocumentTabReleasePosition(
		float CurrentX,
		float CurrentY,
		const std::optional<DocumentTabPointerPosition>& LastValidPosition) noexcept
	{
		if (IsUsableDocumentTabPointerPosition(CurrentX, CurrentY))
			return DocumentTabPointerPosition{ CurrentX, CurrentY };
		if (LastValidPosition && IsUsableDocumentTabPointerPosition(LastValidPosition->X, LastValidPosition->Y))
			return LastValidPosition;
		return std::nullopt;
	}

	[[nodiscard]] inline float CalculateDocumentTabDragDistanceSquared(
		DocumentTabPointerPosition Start,
		const std::optional<DocumentTabPointerPosition>& CurrentPosition,
		const std::optional<DocumentTabPointerPosition>& LastValidPosition,
		float MaximumObservedDistanceSquared) noexcept
	{
		float DistanceSquared = std::isfinite(MaximumObservedDistanceSquared) && MaximumObservedDistanceSquared > 0.0f
			? MaximumObservedDistanceSquared : 0.0f;
		if (!IsUsableDocumentTabPointerPosition(Start.X, Start.Y))
			return DistanceSquared;

		const auto IncludeDistance = [&Start, &DistanceSquared](
			const std::optional<DocumentTabPointerPosition>& Position)
		{
			if (!Position || !IsUsableDocumentTabPointerPosition(Position->X, Position->Y))
				return;
			const float DeltaX = Position->X - Start.X;
			const float DeltaY = Position->Y - Start.Y;
			const float Candidate = DeltaX * DeltaX + DeltaY * DeltaY;
			if (std::isfinite(Candidate) && Candidate > DistanceSquared)
				DistanceSquared = Candidate;
		};
		IncludeDistance(CurrentPosition);
		IncludeDistance(LastValidPosition);
		return DistanceSquared;
	}

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

	[[nodiscard]] constexpr bool CanCreateNativeHostFromDocumentTabDrag(
		bool SourceIsMainHost,
		bool SourceIsNativeHost) noexcept
	{
		return SourceIsMainHost || SourceIsNativeHost;
	}

	[[nodiscard]] constexpr DocumentTabReleaseAction ResolveDocumentTabReleaseAction(
		bool MouseReleased,
		bool HasActiveTabDrag,
		bool TransferQueued,
		float DragDistanceSquared,
		float DragThreshold,
		bool CanCreateNativeHost,
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
		return CanCreateNativeHost
			? DocumentTabReleaseAction::CreateNativeHost
			: DocumentTabReleaseAction::None;
	}
}
