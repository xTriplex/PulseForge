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
		DocumentTabPointerPosition Minimum;
		DocumentTabPointerPosition Maximum;
		size_t BeforeIndex = 0;
		size_t AfterIndex = 0;
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
		DocumentTabPointerPosition PointerPosition;
		uint32_t HoveredViewportID = 0;
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
		void BeginFrame() noexcept;
		void RegisterDropTarget(EditorDocumentTabDropTarget Target);
		[[nodiscard]] std::optional<EditorDocumentTabDropLocation> FindDropTarget(
			DocumentTabPointerPosition Position, uint32_t HoveredViewportID) const;

		void BeginDrag(const EditorDocumentIdentity& Identity, std::optional<DocumentTabPointerPosition> StartPosition,
			DocumentTabPointerPosition CurrentPosition);
		void CancelDrag() noexcept;
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
	};
} // namespace PulseForgeEditor
