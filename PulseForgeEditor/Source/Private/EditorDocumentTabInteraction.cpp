#include "Editor/EditorDocumentTabInteraction.h"

namespace PulseForgeEditor
{
	void EditorDocumentTabInteraction::BeginFrame() noexcept
	{
		m_DropTargets.clear();
	}

	void EditorDocumentTabInteraction::RegisterDropTarget(EditorDocumentTabDropTarget Target)
	{
		m_DropTargets.push_back(std::move(Target));
	}

	std::optional<EditorDocumentTabDropLocation> EditorDocumentTabInteraction::FindDropTarget(
		DocumentTabPointerPosition Position, uint32_t HoveredViewportID) const
	{
		std::optional<EditorDocumentTabDropLocation> Result;
		for (const EditorDocumentTabDropTarget& Target : m_DropTargets)
		{
			if (HoveredViewportID != 0 && Target.ViewportID != HoveredViewportID) continue;
			if (Position.X < Target.Minimum.X || Position.X >= Target.Maximum.X || Position.Y < Target.Minimum.Y ||
				Position.Y >= Target.Maximum.Y)
				continue;

			if (Result) return std::nullopt;
			const float Midpoint = (Target.Minimum.X + Target.Maximum.X) * 0.5f;
			const size_t Index = Target.BeforeIndex != Target.AfterIndex && Position.X >= Midpoint ? Target.AfterIndex : Target.BeforeIndex;
			Result.emplace(EditorDocumentTabDropLocation{ Target.HostID, Index });
		}
		return Result;
	}

	void EditorDocumentTabInteraction::BeginDrag(const EditorDocumentIdentity& Identity,
		std::optional<DocumentTabPointerPosition> StartPosition, DocumentTabPointerPosition CurrentPosition)
	{
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
		CancelDrag();

		const auto ReleasePosition = ResolveDocumentTabReleasePosition(Frame.PointerPosition.X, Frame.PointerPosition.Y, LastValidPosition);
		if (!ReleasePosition) return std::nullopt;

		const DocumentTabPointerPosition DragStart = StartPosition.value_or(*ReleasePosition);
		if (!IsUsableDocumentTabPointerPosition(DragStart.X, DragStart.Y)) return std::nullopt;

		const float DragDistanceSquared =
			CalculateDocumentTabDragDistanceSquared(DragStart, CurrentPosition, LastValidPosition, Frame.MaximumObservedDistanceSquared);
		const bool TransferQueued = m_PendingTransfer && m_PendingTransfer->Identity == Identity;
		const auto DropLocation = CurrentPosition ? FindDropTarget(*CurrentPosition, Frame.HoveredViewportID) : std::nullopt;
		const bool CanCreateNativeHost =
			CanDetachDocumentTabToNativeHost(Frame.SourceIsMainHost, Frame.SourceIsNativeHost, Frame.HoveredViewportID == 0);
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
