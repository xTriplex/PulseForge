#pragma once

#include "Editor/EditorDocumentManager.h"
#include "Editor/EditorDocumentTabPolicy.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace PulseForgeEditor
{
	struct EditorDocumentTabDropTarget
	{
		std::string HostID;
		uint32_t ViewportID = 0;
		uint32_t RootWindowID = 0;
		DocumentTabPointerPosition Minimum;
		DocumentTabPointerPosition Maximum;
		size_t BeforeIndex = 0;
		size_t AfterIndex = 0;
		bool CanReceiveDrop = true;
	};

	struct EditorDocumentTabDropLocation
	{
		std::string HostID;
		size_t Index = 0;
	};

	struct EditorDocumentTransferRequest
	{
		EditorDocumentIdentity Identity;
		std::string DestinationHostID;
		size_t DestinationIndex = 0;
		bool CreateNativeHost = false;
		std::optional<EditorDocumentHost::WindowBounds> NativeHostBounds;
	};

	struct EditorDocumentTabDragFrame
	{
		bool LeftButtonReleased = false;
		bool LeftButtonDown = false;
		bool ImGuiPayloadActive = false;
		bool HasReliableHoveredViewport = false;
		DocumentTabPointerPosition PointerPosition;
		uint32_t HoveredViewportID = 0;
		uint32_t HoveredRootWindowID = 0;
		float MaximumObservedDistanceSquared = 0.0f;
		float DragThreshold = 0.0f;
		bool SourceIsMainHost = false;
		bool SourceIsNativeHost = false;
	};

	struct EditorDocumentTabDragRelease
	{
		EditorDocumentIdentity Identity;
		DocumentTabReleaseAction Action = DocumentTabReleaseAction::None;
		std::optional<EditorDocumentTabDropLocation> DropLocation;
		DocumentTabPointerPosition ReleasePosition;
	};

	class EditorDocumentTabInteraction final
	{
	public:
		void BeginFrame(bool LeftButtonClicked = false) noexcept;
		void RegisterDropTarget(EditorDocumentTabDropTarget Target);
		[[nodiscard]] std::optional<EditorDocumentTabDropLocation> FindDropTarget(
			DocumentTabPointerPosition Position, bool HasReliableHoveredViewport, uint32_t HoveredViewportID,
			uint32_t HoveredRootWindowID) const;

		void BeginDrag(const EditorDocumentIdentity& Identity, std::optional<DocumentTabPointerPosition> StartPosition,
			DocumentTabPointerPosition CurrentPosition);
		void CancelDrag() noexcept;
		void CancelDragUntilNewPress() noexcept;
		[[nodiscard]] bool IsCancelledUntilNewPress() const noexcept;
		[[nodiscard]] std::optional<EditorDocumentIdentity> ActiveDragIdentity() const noexcept;
		[[nodiscard]] std::optional<EditorDocumentTabDragRelease> UpdateDrag(const EditorDocumentTabDragFrame& Frame);

		[[nodiscard]] bool QueueTransfer(EditorDocumentTransferRequest Request);
		[[nodiscard]] std::optional<EditorDocumentTransferRequest> TakePendingTransfer() noexcept;

	private:
		std::vector<EditorDocumentTabDropTarget> m_DropTargets;
		std::optional<EditorDocumentTransferRequest> m_PendingTransfer;
		std::optional<EditorDocumentIdentity> m_ActiveDrag;
		std::optional<DocumentTabPointerPosition> m_DragStart;
		std::optional<DocumentTabPointerPosition> m_LastValidDragPosition;
		bool m_CancelledUntilNewPress = false;
	};
} // namespace PulseForgeEditor
