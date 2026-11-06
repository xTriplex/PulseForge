#include "Editor/EditorDocumentTabInteraction.h"

#include <algorithm>

namespace PulseForgeEditor
{
	void EditorDocumentTabInteraction::BeginFrame(bool LeftButtonClicked) noexcept
	{
		m_DropTargets.clear();
		if (LeftButtonClicked) m_CancelledUntilNewPress = false;
	}

	void EditorDocumentTabInteraction::RegisterDropTarget(EditorDocumentTabDropTarget Target)
	{
		m_DropTargets.push_back(std::move(Target));
	}

	std::optional<EditorDocumentTabDropPreview> EditorDocumentTabInteraction::ResolveDropPreview(
		const EditorDocumentTabDragFrame& Frame) const
	{
		if (!m_ActiveDrag || m_ActiveDrag->Kind == EditorDocumentKind::Level ||
			!CanCreateNativeHostFromDocumentTabDrag(Frame.SourceIsMainHost, Frame.SourceIsNativeHost) ||
			(!Frame.LeftButtonDown && !Frame.LeftButtonReleased) || !Frame.HasReliableHoveredViewport ||
			Frame.HoveredViewportID == 0 || Frame.HoveredRootWindowID == 0 ||
			!IsUsableDocumentTabPointerPosition(Frame.PointerPosition.X, Frame.PointerPosition.Y))
			return std::nullopt;

		const float DragDistanceSquared = CalculateDocumentTabDragDistanceSquared(
			m_DragStart.value_or(Frame.PointerPosition), Frame.PointerPosition, m_LastValidDragPosition,
			Frame.MaximumObservedDistanceSquared);
		if (!std::isfinite(Frame.DragThreshold) || Frame.DragThreshold < 0.0f ||
			DragDistanceSquared < Frame.DragThreshold * Frame.DragThreshold)
			return std::nullopt;

		std::optional<EditorDocumentTabDropPreview> Result;
		for (const EditorDocumentTabDropTarget& Target : m_DropTargets)
		{
			if (!Target.CanReceiveDrop || Target.ViewportID != Frame.HoveredViewportID ||
				Target.RootWindowID != Frame.HoveredRootWindowID)
				continue;
			const DocumentTabPointerPosition VisibleMinimum{ std::max(Target.Minimum.X, Target.ClipMinimum.X),
				std::max(Target.Minimum.Y, Target.ClipMinimum.Y) };
			const DocumentTabPointerPosition VisibleMaximum{ std::min(Target.Maximum.X, Target.ClipMaximum.X),
				std::min(Target.Maximum.Y, Target.ClipMaximum.Y) };
			if (!IsUsableDocumentTabPointerPosition(VisibleMinimum.X, VisibleMinimum.Y) ||
				!IsUsableDocumentTabPointerPosition(VisibleMaximum.X, VisibleMaximum.Y) ||
				VisibleMinimum.X >= VisibleMaximum.X || VisibleMinimum.Y >= VisibleMaximum.Y ||
				Frame.PointerPosition.X < VisibleMinimum.X || Frame.PointerPosition.X >= VisibleMaximum.X ||
				Frame.PointerPosition.Y < VisibleMinimum.Y || Frame.PointerPosition.Y >= VisibleMaximum.Y)
				continue;

			const float Midpoint = (Target.Minimum.X + Target.Maximum.X) * 0.5f;
			const bool IsAppend = Target.BeforeIndex == Target.AfterIndex;
			const size_t Index = !IsAppend && Frame.PointerPosition.X >= Midpoint ? Target.AfterIndex : Target.BeforeIndex;
			const float Edge = IsAppend || Index == Target.BeforeIndex ? Target.Minimum.X : Target.Maximum.X;
			const float InsertionX = std::clamp(Edge, VisibleMinimum.X, VisibleMaximum.X);
			if (Result) return std::nullopt;
			Result.emplace(EditorDocumentTabDropPreview{ *m_ActiveDrag, { Target.HostID, Index }, VisibleMinimum,
				VisibleMaximum, InsertionX, IsAppend });
		}
		return Result;
	}

	void EditorDocumentTabInteraction::BeginDrag(const EditorDocumentIdentity& Identity,
		std::optional<DocumentTabPointerPosition> StartPosition, DocumentTabPointerPosition CurrentPosition)
	{
		if (m_CancelledUntilNewPress) return;
		if (!m_ActiveDrag || *m_ActiveDrag != Identity)
		{
			m_DragStart.reset();
			m_LastValidDragPosition.reset();
		}
		m_ActiveDrag = Identity;
		if (StartPosition) m_DragStart = StartPosition;
		if (IsUsableDocumentTabPointerPosition(CurrentPosition.X, CurrentPosition.Y)) m_LastValidDragPosition = CurrentPosition;
	}

	void EditorDocumentTabInteraction::CancelDrag() noexcept
	{
		m_ActiveDrag.reset();
		m_DragStart.reset();
		m_LastValidDragPosition.reset();
	}

	void EditorDocumentTabInteraction::CancelDragUntilNewPress() noexcept
	{
		CancelDrag();
		m_CancelledUntilNewPress = true;
	}

	bool EditorDocumentTabInteraction::IsCancelledUntilNewPress() const noexcept
	{
		return m_CancelledUntilNewPress;
	}

	std::optional<EditorDocumentIdentity> EditorDocumentTabInteraction::ActiveDragIdentity() const noexcept
	{
		return m_ActiveDrag;
	}

	std::optional<EditorDocumentTabDragRelease> EditorDocumentTabInteraction::UpdateDrag(const EditorDocumentTabDragFrame& Frame)
	{
		const bool CurrentPositionIsValid = IsUsableDocumentTabPointerPosition(Frame.PointerPosition.X, Frame.PointerPosition.Y);
		const std::optional<DocumentTabPointerPosition> CurrentPosition =
			CurrentPositionIsValid ? std::optional<DocumentTabPointerPosition>(Frame.PointerPosition) : std::nullopt;

		if (!Frame.LeftButtonReleased)
		{
			if (m_ActiveDrag && Frame.LeftButtonDown && CurrentPosition) m_LastValidDragPosition = CurrentPosition;
			if (!Frame.LeftButtonDown && !Frame.ImGuiPayloadActive) CancelDrag();
			return std::nullopt;
		}

		if (!m_ActiveDrag) return std::nullopt;

		const EditorDocumentIdentity Identity = *m_ActiveDrag;
		const std::optional<DocumentTabPointerPosition> LastValidPosition = m_LastValidDragPosition;
		const std::optional<DocumentTabPointerPosition> StartPosition = m_DragStart;
		const auto DropPreview = ResolveDropPreview(Frame);
		CancelDrag();

		const auto ReleasePosition = ResolveDocumentTabReleasePosition(Frame.PointerPosition.X, Frame.PointerPosition.Y, LastValidPosition);
		if (!ReleasePosition) return std::nullopt;

		const DocumentTabPointerPosition DragStart = StartPosition.value_or(*ReleasePosition);
		if (!IsUsableDocumentTabPointerPosition(DragStart.X, DragStart.Y)) return std::nullopt;

		const float DragDistanceSquared =
			CalculateDocumentTabDragDistanceSquared(DragStart, CurrentPosition, LastValidPosition, Frame.MaximumObservedDistanceSquared);
		const bool TransferQueued = m_PendingTransfer && m_PendingTransfer->Identity == Identity;
		const auto DropLocation = DropPreview ? std::optional<EditorDocumentTabDropLocation>(DropPreview->Location) : std::nullopt;
		const bool CanCreateNativeHost = CanCreateNativeHostFromDocumentTabDrag(
			Frame.SourceIsMainHost, Frame.SourceIsNativeHost);
		const DocumentTabReleaseAction Action = ResolveDocumentTabReleaseAction(true, true, TransferQueued, DragDistanceSquared,
			Frame.DragThreshold, CanCreateNativeHost, Identity.Kind != EditorDocumentKind::Level, DropLocation.has_value());
		return EditorDocumentTabDragRelease{ Identity, Action, DropLocation, *ReleasePosition };
	}

	bool EditorDocumentTabInteraction::QueueTransfer(EditorDocumentTransferRequest Request)
	{
		if (m_PendingTransfer) return false;
		m_PendingTransfer = std::move(Request);
		return true;
	}

	std::optional<EditorDocumentTransferRequest> EditorDocumentTabInteraction::TakePendingTransfer() noexcept
	{
		auto Request = std::move(m_PendingTransfer);
		m_PendingTransfer.reset();
		return Request;
	}
} // namespace PulseForgeEditor
