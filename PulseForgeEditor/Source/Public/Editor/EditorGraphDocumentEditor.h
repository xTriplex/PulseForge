#pragma once

#include "Editor/EditorDocumentEditorRegistry.h"
#include "Editor/EditorDocumentWorkspace.h"
#include "Editor/EditorGraphModel.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace PulseForgeEditor
{
	struct EditorGraphDocumentEditorTestAccess;
	struct EditorGraphPropertyEditEvent;
	struct EditorGraphPropertyEditResult;
	struct EditorGraphCanvasLinkRequest
	{
		uintptr_t StartPin = 0;
		uintptr_t EndPin = 0;
	};

	struct EditorGraphScreenPoint
	{
		double X = 0.0;
		double Y = 0.0;
	};

	struct EditorGraphCanvasPoint
	{
		double X = 0.0;
		double Y = 0.0;
	};

	struct EditorGraphCanvasPinMarkerGeometry
	{
		EditorGraphCanvasPoint Minimum;
		EditorGraphCanvasPoint Maximum;
		EditorGraphCanvasPoint Anchor;
	};

	struct EditorGraphCanvasRect
	{
		EditorGraphCanvasPoint Minimum;
		EditorGraphCanvasPoint Maximum;
	};

	struct EditorGraphCanvasPinPresentation
	{
		PulseForge::UUID PinID;
		EditorGraphPinDirection Direction = EditorGraphPinDirection::Input;
		bool Connected = false;
		bool Visible = true;
		EditorGraphCanvasPoint LinkDirection;
		EditorGraphCanvasPinMarkerGeometry Marker;
		std::optional<EditorGraphCanvasRect> HitBounds;
		std::optional<EditorGraphCanvasRect> Label;
		std::optional<EditorGraphCanvasRect> PropertyControl;
	};

	struct EditorGraphCanvasNodePresentation
	{
		PulseForge::UUID NodeID;
		bool IsReroute = false;
		EditorGraphCanvasPoint Position;
		EditorGraphCanvasPoint Size;
		size_t HiddenPinCount = 0;
		double Zoom = 1.0;
		double InputColumnWidth = 0.0;
		double OutputColumnWidth = 0.0;
		std::optional<EditorGraphCanvasRect> PinsButtonScreenBounds;
		bool PinsButtonHovered = false;
		bool PinsButtonActivated = false;
		std::vector<EditorGraphCanvasPinPresentation> Pins;
	};

	struct EditorGraphPinVisibilityPopupItemPresentation
	{
		PulseForge::UUID PinID;
		EditorGraphCanvasRect Bounds;
		bool Visible = true;
		bool Enabled = true;
	};

	struct EditorGraphPinVisibilityPopupPresentation
	{
		PulseForge::UUID NodeID;
		std::optional<EditorGraphCanvasRect> TriggerScreenBounds;
		std::optional<EditorGraphCanvasRect> PopupScreenBounds;
		std::optional<EditorGraphCanvasRect> ShowAllScreenBounds;
		std::optional<EditorGraphCanvasRect> HideUnusedScreenBounds;
		std::optional<EditorGraphCanvasRect> ResetVisibilityScreenBounds;
		std::optional<EditorGraphCanvasRect> PinsSubmenuScreenBounds;
		std::vector<EditorGraphPinVisibilityPopupItemPresentation> PinItems;
		bool ContextValid = false;
		bool OpenSubmitted = false;
		bool PopupVisible = false;
		bool PopupOpenAfterSubmit = false;
		bool PopupWindowHovered = false;
		bool NoMove = false;
		bool ShowAllHovered = false;
		bool HideUnusedHovered = false;
		bool HideUnusedEnabled = true;
		bool ResetVisibilityHovered = false;
		bool PinsSubmenuHovered = false;
		bool ShowAllActivated = false;
		bool HideUnusedActivated = false;
		bool ResetVisibilityActivated = false;
	};

	struct EditorGraphReconnectPopupPresentation
	{
		PulseForge::UUID ConnectionID;
		std::optional<EditorGraphCanvasRect> PopupScreenBounds;
		std::optional<EditorGraphCanvasRect> ReconnectInputBounds;
		std::optional<EditorGraphCanvasRect> ReconnectOutputBounds;
		bool ReconnectInputHovered = false;
		bool ReconnectOutputHovered = false;
		PulseForge::UUID HoveredConnectionAtRightRelease;
		bool ContextValid = false;
		bool LinkContextRequestSeen = false;
		bool PopupVisible = false;
		bool PopupOpenAfterSubmit = false;
		bool NoMove = false;
		bool NoResize = false;
	};

	struct EditorGraphPinMovePresentation
	{
		struct Curve final
		{
			PulseForge::UUID ConnectionID;
			EditorGraphCanvasPoint Start;
			EditorGraphCanvasPoint End;
		};
		bool Active = false;
		bool CtrlDrag = false;
		bool Dragging = false;
		bool AwaitingTargetClick = false;
		PulseForge::UUID SourcePin;
		PulseForge::UUID TargetPin;
		EditorGraphPinDirection Direction = EditorGraphPinDirection::Input;
		size_t MovingConnectionCount = 0;
		size_t DisplacedConnectionCount = 0;
		bool TargetAllowed = false;
		bool TargetNoOp = false;
		std::string Feedback;
		size_t PreviewDecisionEvaluations = 0;
		size_t CommitCount = 0;
		bool PreviewSubmittedOnCanvasUnderlay = false;
		std::vector<Curve> PreviewCurves;
	};

	struct EditorGraphCanvasDocumentContext
	{
		EditorDocumentIdentity Document;
		uint64_t SessionGeneration = 0;
		std::string HostID;
		PulseForge::UUID GraphID;
	};

	struct EditorGraphPendingPinRelease
	{
		bool Valid = false;
		EditorGraphCanvasDocumentContext Context;
		PulseForge::UUID SourcePin;
		EditorGraphCanvasPoint Position;
	};

	struct EditorGraphAcceptedPinRelease
	{
		PulseForge::UUID SourcePin;
		EditorGraphCanvasPoint Position;
	};

	struct EditorGraphCanvasLinkRequestResult
	{
		bool SawRequest = false;
		bool Accepted = false;
		EditorGraphConnectionDecision Decision;
	};

	struct EditorGraphCanvasCreateRequestResult
	{
		bool SawLinkRequest = false;
		bool AcceptedLink = false;
		EditorGraphConnectionDecision LinkDecision;
		bool SawNodeRequest = false;
		bool AcceptedNode = false;
		uintptr_t SourcePin = 0;
	};

	struct EditorGraphWireRoutePresentation
	{
		bool Active = false;
		bool OpenEnded = false;
		bool KeyPressed = false;
		bool InputEligible = false;
		bool Focused = false;
		bool LeftDown = false;
		bool EscapeDown = false;
		bool TextInputOwned = false;
		bool PopupOwned = false;
		bool MouseDragging = false;
		bool HasPendingStart = false;
		bool PointerInCanvasBackground = false;
		bool ContextualSearchOpen = false;
		bool SearchPlacementAwaitingPinAnchor = false;
		bool ReleaseCommitRequested = false;
		bool ReleaseCommitSucceeded = false;
		PulseForge::UUID ReleaseTargetPin;
		std::string Feedback;
		uintptr_t HoveredPin = 0;
		uintptr_t HoveredNode = 0;
		PulseForge::UUID SourcePin;
		EditorGraphVector3 SearchDropPoint;
		EditorGraphScreenPoint SearchDropScreen;
		EditorGraphCanvasRect FirstSearchResultBounds;
		std::vector<std::string> SearchActionTypeKeys;
		std::vector<EditorGraphVector3> KnotPositions;
		std::vector<EditorGraphCanvasPoint> PreviewEndpoints;
	};

	struct EditorGraphCanvasPinInput
	{
		PulseForge::UUID HoveredPin;
		bool Focused = false;
		bool Alt = false;
		bool LeftClicked = false;
		bool TextInputOwned = false;
		bool PopupOpen = false;
		bool EmbeddedWidgetHovered = false;
	};

	struct EditorGraphStraightenInput
	{
		bool Focused = false;
		bool CanvasActive = false;
		bool KeyPressed = false;
		bool TextInputOwned = false;
		bool OtherInputOwned = false;
		bool PopupOpen = false;
		bool MouseButtonDown = false;
		bool NodeMoveTransaction = false;
		bool GraphTransaction = false;
		bool PropertyEditActive = false;
		bool PropertyWidgetHovered = false;
	};

	struct EditorGraphNodeSearchNavigationInput
	{
		size_t ResultCount = 0;
		int SelectedResult = 0;
		bool SearchChanged = false;
		bool MoveUp = false;
		bool MoveDown = false;
		bool Activate = false;
		bool Cancel = false;
	};

	struct EditorGraphNodeSearchNavigationResult
	{
		int SelectedResult = 0;
		std::optional<size_t> ActivateResult;
		bool Cancelled = false;
	};

	namespace GraphCanvasDetail
	{
		[[nodiscard]] inline EditorGraphCanvasPoint MakeEditorGraphCanvasPoint(double X, double Y)
		{
			return { X, Y };
		}

		[[nodiscard]] inline EditorGraphCanvasPinMarkerGeometry MakePinMarkerGeometry(
			EditorGraphCanvasPoint Center, double Width, double Height)
		{
			const double HalfWidth = Width * 0.5;
			const double HalfHeight = Height * 0.5;
			return { { Center.X - HalfWidth, Center.Y - HalfHeight },
				{ Center.X + HalfWidth, Center.Y + HalfHeight }, Center };
		}

		template <typename SetBounds, typename SetPivot>
		inline void ApplyPinMarkerGeometry(const EditorGraphCanvasPinMarkerGeometry& Geometry,
			SetBounds&& SetBoundsCallback, SetPivot&& SetPivotCallback)
		{
			std::forward<SetBounds>(SetBoundsCallback)(Geometry.Minimum, Geometry.Maximum);
			std::forward<SetPivot>(SetPivotCallback)(Geometry.Anchor);
		}

		[[nodiscard]] inline EditorGraphVector3 ToEditorGraphPosition(EditorGraphCanvasPoint Point)
		{
			// The pinned canvas floors SetNodePosition bounds. Match that placement anchor
			// before model publication so the first canvas sync is not a second edit.
			return { std::floor(Point.X), std::floor(Point.Y), 0.0 };
		}

		template <typename ScreenToCanvas>
		[[nodiscard]] inline EditorGraphCanvasPoint ConvertEditorGraphScreenPoint(
			EditorGraphScreenPoint Screen, ScreenToCanvas&& Convert)
		{
			return std::forward<ScreenToCanvas>(Convert)(Screen);
		}

		[[nodiscard]] inline std::optional<EditorGraphAcceptedPinRelease> ConsumePinRelease(
			EditorGraphPendingPinRelease& Pending,
			const EditorGraphCanvasDocumentContext& Current,
			PulseForge::UUID SourcePin)
		{
			if (!Pending.Valid) return std::nullopt;
			const auto Captured = Pending;
			Pending = {};
			if (SourcePin.IsNil() || Captured.SourcePin != SourcePin || Captured.Context.Document != Current.Document ||
				Captured.Context.SessionGeneration != Current.SessionGeneration ||
				Captured.Context.HostID != Current.HostID || Captured.Context.GraphID != Current.GraphID)
				return std::nullopt;
			return EditorGraphAcceptedPinRelease{ SourcePin, Captured.Position };
		}

		[[nodiscard]] inline std::optional<PulseForge::UUID> ResolveAltPinUnlink(
			const EditorGraphCanvasPinInput& Input)
		{
			if (!Input.Focused || !Input.Alt || !Input.LeftClicked || Input.TextInputOwned ||
				Input.PopupOpen || Input.EmbeddedWidgetHovered || Input.HoveredPin.IsNil()) return std::nullopt;
			return Input.HoveredPin;
		}

		[[nodiscard]] inline bool ShouldStraightenSelection(const EditorGraphStraightenInput& Input)
		{
			return Input.Focused && Input.CanvasActive && Input.KeyPressed && !Input.TextInputOwned &&
				!Input.OtherInputOwned && !Input.PopupOpen && !Input.MouseButtonDown &&
				!Input.NodeMoveTransaction && !Input.GraphTransaction && !Input.PropertyEditActive &&
				!Input.PropertyWidgetHovered;
		}

		[[nodiscard]] inline EditorGraphNodeSearchNavigationResult ResolveNodeSearchNavigation(
			const EditorGraphNodeSearchNavigationInput& Input)
		{
			EditorGraphNodeSearchNavigationResult Result;
			if (Input.ResultCount == 0)
			{
				Result.Cancelled = Input.Cancel;
				return Result;
			}
			const int Count = static_cast<int>(Input.ResultCount);
			Result.SelectedResult = Input.SearchChanged ? 0 : std::clamp(Input.SelectedResult, 0, Count - 1);
			if (Input.MoveDown) Result.SelectedResult = (Result.SelectedResult + 1) % Count;
			if (Input.MoveUp) Result.SelectedResult = (Result.SelectedResult + Count - 1) % Count;
			Result.Cancelled = Input.Cancel;
			if (!Input.Cancel && Input.Activate) Result.ActivateResult = static_cast<size_t>(Result.SelectedResult);
			return Result;
		}

		template <typename EndCreate>
		struct EditorGraphCreateEnd final
		{
			EndCreate& End;
			~EditorGraphCreateEnd() { End(); }
		};

		template <typename BeginCreate, typename EndCreate, typename Query, typename Resolve,
			typename Accept, typename Reject>
		[[nodiscard]] EditorGraphCanvasLinkRequestResult ProcessEditorGraphCanvasLinkRequestFrame(
			BeginCreate&& Begin, EndCreate&& End, Query&& QueryRequest, Resolve&& ResolveRequest,
			Accept&& AcceptRequest, Reject&& RejectRequest)
		{
			const bool Active = Begin();
			EditorGraphCreateEnd<std::remove_reference_t<EndCreate>> EndGuard{ End };
			if (!Active) return {};

			const auto Request = QueryRequest();
			if (!Request) return {};

			EditorGraphCanvasLinkRequestResult Result;
			Result.SawRequest = true;
			Result.Decision = ResolveRequest(*Request);
			if (Result.Decision.Allowed)
				Result.Accepted = AcceptRequest();
			else RejectRequest();
			return Result;
		}

		template <typename BeginCreate, typename EndCreate, typename QueryLink, typename ResolveLink,
			typename AcceptLink, typename RejectLink, typename QueryNode, typename ResolveNode,
			typename AcceptNode, typename RejectNode>
		[[nodiscard]] EditorGraphCanvasCreateRequestResult ProcessEditorGraphCanvasCreateRequestFrame(
			BeginCreate&& Begin, EndCreate&& End, QueryLink&& QueryLinkRequest,
			ResolveLink&& ResolveLinkRequest, AcceptLink&& AcceptLinkRequest,
			RejectLink&& RejectLinkRequest, QueryNode&& QueryNodeRequest,
			ResolveNode&& ResolveNodeRequest, AcceptNode&& AcceptNodeRequest,
			RejectNode&& RejectNodeRequest, bool ObserveOnly = false,
			bool RetireObservedNode = false)
		{
			const bool Active = Begin();
			EditorGraphCreateEnd<std::remove_reference_t<EndCreate>> EndGuard{ End };
			if (!Active) return {};

			EditorGraphCanvasCreateRequestResult Result;
			if (const auto Link = QueryLinkRequest())
			{
				Result.SawLinkRequest = true;
				Result.LinkDecision = ResolveLinkRequest(*Link);
				if (ObserveOnly)
				{
					if (RetireObservedNode) RejectNodeRequest();
					return Result;
				}
				if (Result.LinkDecision.Allowed)
					Result.AcceptedLink = AcceptLinkRequest();
				else RejectLinkRequest();
				return Result;
			}

			if (const auto NodePin = QueryNodeRequest())
			{
				Result.SawNodeRequest = true;
				Result.SourcePin = *NodePin;
				if (ObserveOnly)
				{
					// A route gesture takes over the native background-create stage.
					// Retire that stage now so its accepted drag cannot become a node
					// move or a delayed create after the route is cancelled.
					if (RetireObservedNode) RejectNodeRequest();
					return Result;
				}
				if (ResolveNodeRequest(*NodePin)) Result.AcceptedNode = AcceptNodeRequest();
				else RejectNodeRequest();
			}
			return Result;
		}
	}

	enum class EditorGraphFixtureKind : uint8_t
	{
		MaterialExpression,
		GameplayEvent
	};

	struct EditorGraphFixtureLocation
	{
		PulseForge::UUID GraphID;
		std::filesystem::path ProjectRelativePath;
		std::string DisplayName;
	};

	class EditorGraphDocumentEditor final
	{
	public:
		EditorGraphDocumentEditor(EditorDocumentManager& Documents, EditorDocumentEditorRegistry& Editors,
			std::function<void()> PresentationChanged = {});

		[[nodiscard]] std::expected<void, std::string> Register();
		// Schemas are registered before Register() and remain immutable for all document sessions.
		// Per-session action-discovery caches therefore key catalog state by stable schema key/version.
		[[nodiscard]] std::expected<void, std::string> RegisterSchema(EditorGraphSchema Schema);
		[[nodiscard]] std::expected<EditorGraphFixtureLocation, std::string> CreateFixture(
			const std::filesystem::path& ProjectRoot, EditorGraphFixtureKind Kind) const;
		[[nodiscard]] static std::filesystem::path GetFixturePath(const PulseForge::UUID& GraphID);
		[[nodiscard]] bool Open(EditorDocumentSession& Session, const EditorDocumentEditorContext& Context) const;
		[[nodiscard]] bool Save(EditorDocumentSession& Session) const;
		[[nodiscard]] std::expected<PulseForge::UUID, std::string> AddNode(
			EditorDocumentSession& Session, std::string_view NodeTypeKey, double CanvasX, double CanvasY) const;
		[[nodiscard]] std::vector<EditorDocumentWorkspaceState::GraphPinVisibility> CapturePinVisibility(
			const EditorDocumentSession& Session) const;
		[[nodiscard]] std::vector<EditorDocumentWorkspaceState::GraphPinVisibility> CaptureWorkspacePinVisibility() const;
		void SetWorkspacePinVisibility(
			std::vector<EditorDocumentWorkspaceState::GraphPinVisibility> Settings);
		void RestorePinVisibility(EditorDocumentSession& Session,
			const std::vector<EditorDocumentWorkspaceState::GraphPinVisibility>& Settings);

	private:
		friend struct EditorGraphDocumentEditorTestAccess;

		[[nodiscard]] bool IsEligible(const EditorDocumentEditorContext& Context) const;
		[[nodiscard]] std::expected<std::unique_ptr<EditorDocumentEditorState>, std::string> LoadSession(
			const EditorDocumentEditorContext& Context) const;
		void DrawWorkspace(EditorDocumentSession& Session);
		[[nodiscard]] EditorGraphPropertyEditResult ProcessPropertyEditEvents(EditorDocumentSession& Session,
			const std::vector<EditorGraphPropertyEditEvent>& Events, bool CancelGesture);
		[[nodiscard]] bool SaveSession(EditorDocumentSession& Session, uint64_t SavedRevision) const;
		[[nodiscard]] bool DiscardSession(EditorDocumentSession& Session) const;
		[[nodiscard]] bool HandleFocusedCommand(EditorDocumentSession& Session, EditorDocumentCommand Command) const;
		[[nodiscard]] bool UpdateAssetLocation(EditorDocumentSession& Session,
			const std::filesystem::path& Path, std::string_view DisplayName) const;
		[[nodiscard]] std::expected<void, std::string> PrepareClose(const EditorDocumentCloseContext& Context) const;
		[[nodiscard]] bool ReloadState(EditorDocumentSession& Session) const;
		void SetGraphForTesting(EditorDocumentSession& Session, EditorGraph Graph, EditorGraphSchema Schema);
		void SetCanvasSettingsForTesting(EditorDocumentSession& Session, std::string Settings);
		void CapturePresentationForTesting(EditorDocumentSession& Session, bool Capture);
		[[nodiscard]] std::vector<EditorGraphCanvasNodePresentation> GetPresentationForTesting(
			EditorDocumentSession& Session);
		[[nodiscard]] EditorGraphPinVisibilityPopupPresentation GetPinVisibilityPopupForTesting(
			EditorDocumentSession& Session);
		[[nodiscard]] EditorGraphReconnectPopupPresentation GetReconnectPopupForTesting(
			EditorDocumentSession& Session);
		[[nodiscard]] EditorGraphPinMovePresentation GetPinMoveForTesting(EditorDocumentSession& Session);
		[[nodiscard]] EditorGraphWireRoutePresentation GetWireRouteForTesting(EditorDocumentSession& Session);
		void QueueNodeSearchForTesting(EditorDocumentSession& Session, std::optional<PulseForge::UUID> SourcePin,
			EditorGraphVector3 Placement);
		[[nodiscard]] bool IsCanvasInputSuppressedForTesting(EditorDocumentSession& Session);
		[[nodiscard]] PulseForge::UUID GetLastHoveredPinForTesting(EditorDocumentSession& Session);
		void SetPinHiddenForTesting(EditorDocumentSession& Session, PulseForge::UUID PinID, bool Hidden);
		[[nodiscard]] EditorGraph GetGraphForTesting(EditorDocumentSession& Session);
		void SetSelectedNodesForTesting(EditorDocumentSession& Session, const std::vector<PulseForge::UUID>& NodeIDs);
		[[nodiscard]] std::vector<PulseForge::UUID> GetSelectedNodesForTesting(EditorDocumentSession& Session);
		[[nodiscard]] EditorGraphCanvasPoint ScreenToCanvasForTesting(
			EditorDocumentSession& Session, EditorGraphScreenPoint ScreenPoint);
		[[nodiscard]] EditorGraphScreenPoint CanvasToScreenForTesting(
			EditorDocumentSession& Session, EditorGraphCanvasPoint CanvasPoint);
		[[nodiscard]] PulseForge::UUID GetHoveredConnectionForTesting(EditorDocumentSession& Session);

		EditorDocumentManager& m_Documents;
		EditorDocumentEditorRegistry& m_Editors;
		std::function<void()> m_PresentationChanged;
		std::vector<EditorDocumentWorkspaceState::GraphPinVisibility> m_PinVisibilitySettings;
		EditorGraphSchemaRegistry m_SchemaRegistry;
		bool m_Registered = false;
	};
}
