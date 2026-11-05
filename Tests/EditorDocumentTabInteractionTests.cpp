#include "TestSupport.h"

#include "Editor/EditorDocumentTabInteraction.h"

#include <array>

void RunEditorDocumentTabInteractionTests(TestRunner& Tests)
{
	using namespace PulseForge;
	using namespace PulseForgeEditor;

	const auto Project = UUID::Generate();
	const auto Asset = UUID::Generate();
	PF_CHECK(Tests, Project && Asset);
	if (!Project || !Asset) return;

	const EditorDocumentIdentity Material{ *Project, *Asset, EditorDocumentKind::Material };
	const EditorDocumentIdentity Level{ *Project, UUID{}, EditorDocumentKind::Level };
	EditorDocumentTabInteraction Interaction;
	Interaction.BeginFrame();
	Interaction.RegisterDropTarget({ "main", 10, 100, { 10.0f, 10.0f }, { 110.0f, 50.0f }, 0, 1, true });

	const auto BeforeMidpoint = Interaction.FindDropTarget({ 30.0f, 20.0f }, true, 10, 100);
	const auto AfterMidpoint = Interaction.FindDropTarget({ 90.0f, 20.0f }, true, 10, 100);
	PF_CHECK(Tests, BeforeMidpoint && BeforeMidpoint->HostID == "main" && BeforeMidpoint->Index == 0);
	PF_CHECK(Tests, AfterMidpoint && AfterMidpoint->HostID == "main" && AfterMidpoint->Index == 1);
	PF_CHECK(Tests, !Interaction.FindDropTarget({ 90.0f, 20.0f }, true, 11, 100));
	PF_CHECK(Tests, !Interaction.FindDropTarget({ 90.0f, 20.0f }, false, 10, 100));
	PF_CHECK(Tests, !Interaction.FindDropTarget({ 90.0f, 20.0f }, true, 0, 100));
	PF_CHECK(Tests, !Interaction.FindDropTarget({ 90.0f, 20.0f }, true, 10, 101));
	PF_CHECK(Tests, !Interaction.FindDropTarget({ 110.0f, 20.0f }, true, 10, 100));
	PF_CHECK(Tests, ShouldSuppressTabPressAfterDrag(true, true));
	PF_CHECK(Tests, !ShouldSuppressTabPressAfterDrag(true, false));
	PF_CHECK(Tests, !ShouldSuppressTabPressAfterDrag(false, true));
	PF_CHECK(Tests, CanCreateNativeHostFromDocumentTabDrag(true, false));
	PF_CHECK(Tests, CanCreateNativeHostFromDocumentTabDrag(false, true));
	PF_CHECK(Tests, !CanCreateNativeHostFromDocumentTabDrag(false, false));
	PF_CHECK(Tests, ShouldCaptureDocumentTabDragInput(true, false, true, false, 40.0f, 6.0f));
	PF_CHECK(Tests, ShouldCaptureDocumentTabDragInput(true, false, false, true, 40.0f, 6.0f));
	PF_CHECK(Tests, !ShouldCaptureDocumentTabDragInput(true, false, true, false, 20.0f, 6.0f));
	PF_CHECK(Tests, ShouldCaptureDocumentTabDragInput(false, true, false, true, 40.0f, 6.0f));
	PF_CHECK(Tests, !ShouldCaptureDocumentTabDragInput(false, true, false, false, 40.0f, 6.0f));

	// The hovered viewport and root window decide which strip is physically on top,
	// independent of registration order and overlapping viewport rectangles.
	Interaction.BeginFrame();
	Interaction.RegisterDropTarget({ "main", 10, 100, { 10.0f, 10.0f }, { 110.0f, 50.0f }, 0, 1, true });
	Interaction.RegisterDropTarget({ "native", 11, 200, { 10.0f, 10.0f }, { 110.0f, 50.0f }, 0, 1, true });
	const auto NativeFirst = Interaction.FindDropTarget({ 30.0f, 20.0f }, true, 11, 200);
	PF_CHECK(Tests, NativeFirst && NativeFirst->HostID == "native");
	Interaction.BeginFrame();
	Interaction.RegisterDropTarget({ "native", 11, 200, { 10.0f, 10.0f }, { 110.0f, 50.0f }, 0, 1, true });
	Interaction.RegisterDropTarget({ "main", 10, 100, { 10.0f, 10.0f }, { 110.0f, 50.0f }, 0, 1, true });
	const auto MainFirst = Interaction.FindDropTarget({ 30.0f, 20.0f }, true, 11, 200);
	PF_CHECK(Tests, MainFirst && MainFirst->HostID == "native");
	Interaction.BeginFrame();
	Interaction.RegisterDropTarget({ "incompatible", 12, 300, { 10.0f, 10.0f }, { 110.0f, 50.0f }, 0, 1, false });
	PF_CHECK(Tests, !Interaction.FindDropTarget({ 30.0f, 20.0f }, true, 12, 300));

	const auto ReleaseTo = [&](DocumentTabPointerPosition Position, uint32_t ViewportID, uint32_t RootWindowID,
		bool HasReliableViewport = true, float MaximumDistanceSquared = 4900.0f)
	{
		Interaction.BeginFrame();
		Interaction.BeginDrag(Material, DocumentTabPointerPosition{ 20.0f, 20.0f }, { 20.0f, 20.0f });
		EditorDocumentTabDragFrame Frame;
		Frame.LeftButtonDown = true;
		Frame.PointerPosition = { 40.0f, 20.0f };
		Frame.MaximumObservedDistanceSquared = MaximumDistanceSquared;
		Frame.DragThreshold = 6.0f;
		Frame.SourceIsMainHost = true;
		PF_CHECK(Tests, !Interaction.UpdateDrag(Frame));
		Frame.LeftButtonDown = false;
		Frame.LeftButtonReleased = true;
		Frame.ImGuiPayloadActive = false;
		Frame.HasReliableHoveredViewport = HasReliableViewport;
		Frame.HoveredViewportID = ViewportID;
		Frame.HoveredRootWindowID = RootWindowID;
		Frame.PointerPosition = Position;
		return Interaction.UpdateDrag(Frame);
	};

	// All non-strip locations, including utility controls and the main workspace,
	// intentionally detach. A hidden strip in an overlapping viewport is ignored.
	const std::array<DocumentTabPointerPosition, 7> NonStripReleasePoints = { DocumentTabPointerPosition{ 180.0f, 120.0f },
		DocumentTabPointerPosition{ 240.0f, 220.0f }, DocumentTabPointerPosition{ 300.0f, 320.0f },
		DocumentTabPointerPosition{ 400.0f, 420.0f }, DocumentTabPointerPosition{ 500.0f, 520.0f },
		DocumentTabPointerPosition{ 600.0f, 620.0f }, DocumentTabPointerPosition{ 1200.0f, 120.0f } };
	for (const auto Point : NonStripReleasePoints)
	{
		const auto Detached = ReleaseTo(Point, 10, 100);
		PF_CHECK(Tests, Detached && Detached->Action == DocumentTabReleaseAction::CreateNativeHost && !Detached->DropLocation);
	}

	// A registered strip is eligible only when it is the actual hovered viewport/root
	// and accepts this document. Otherwise release-anywhere falls back to tear-off.
	Interaction.BeginFrame();
	Interaction.RegisterDropTarget({ "main", 10, 100, { 10.0f, 10.0f }, { 110.0f, 50.0f }, 0, 1, true });
	Interaction.BeginDrag(Material, DocumentTabPointerPosition{ 20.0f, 20.0f }, { 20.0f, 20.0f });
	EditorDocumentTabDragFrame Frame;
	Frame.LeftButtonDown = true;
	Frame.PointerPosition = { 40.0f, 20.0f };
	Frame.MaximumObservedDistanceSquared = 400.0f;
	Frame.DragThreshold = 6.0f;
	Frame.SourceIsNativeHost = true;
	PF_CHECK(Tests, !Interaction.UpdateDrag(Frame));
	Frame.LeftButtonDown = false;
	Frame.LeftButtonReleased = true;
	Frame.HasReliableHoveredViewport = true;
	Frame.HoveredViewportID = 10;
	Frame.HoveredRootWindowID = 100;
	Frame.PointerPosition = { 90.0f, 20.0f };
	const auto StripRelease = Interaction.UpdateDrag(Frame);
	PF_CHECK(Tests, StripRelease && StripRelease->Identity == Material);
	PF_CHECK(Tests, StripRelease && StripRelease->Action == DocumentTabReleaseAction::TransferToStrip);
	PF_CHECK(Tests, StripRelease && StripRelease->DropLocation && StripRelease->DropLocation->HostID == "main" &&
				StripRelease->DropLocation->Index == 1);

	// If an overlapping main/native pair reports a main release hit, viewport ordering
	// cannot reroute the native window's strip into the drop decision.
	Interaction.BeginFrame();
	Interaction.RegisterDropTarget({ "native", 11, 200, { 10.0f, 10.0f }, { 110.0f, 50.0f }, 0, 1, true });
	Interaction.BeginDrag(Material, DocumentTabPointerPosition{ 20.0f, 20.0f }, { 20.0f, 20.0f });
	Frame.SourceIsNativeHost = true;
	Frame.HoveredViewportID = 10;
	Frame.HoveredRootWindowID = 100;
	Frame.PointerPosition = { 30.0f, 20.0f };
	const auto ObscuredNativeStrip = Interaction.UpdateDrag(Frame);
	PF_CHECK(Tests, ObscuredNativeStrip && ObscuredNativeStrip->Action == DocumentTabReleaseAction::CreateNativeHost);

	Interaction.BeginFrame();
	Interaction.BeginDrag(Material, DocumentTabPointerPosition{ 20.0f, 20.0f }, { 20.0f, 20.0f });
	EditorDocumentTabDragFrame BelowThresholdFrame;
	BelowThresholdFrame.LeftButtonReleased = true;
	BelowThresholdFrame.PointerPosition = { 22.0f, 20.0f };
	BelowThresholdFrame.SourceIsMainHost = true;
	BelowThresholdFrame.DragThreshold = 6.0f;
	const auto BelowThreshold = Interaction.UpdateDrag(BelowThresholdFrame);
	PF_CHECK(Tests, BelowThreshold && BelowThreshold->Action == DocumentTabReleaseAction::None);
	const auto UnreliableViewport = ReleaseTo({ 90.0f, 20.0f }, 10, 100, false);
	PF_CHECK(Tests, UnreliableViewport && UnreliableViewport->Action == DocumentTabReleaseAction::CreateNativeHost);

	Interaction.BeginFrame();
	Interaction.BeginDrag(Material, DocumentTabPointerPosition{ 20.0f, 20.0f }, { 20.0f, 20.0f });
	EditorDocumentTabDragFrame MissingPosition;
	MissingPosition.LeftButtonDown = true;
	MissingPosition.PointerPosition = { 80.0f, 40.0f };
	MissingPosition.MaximumObservedDistanceSquared = 4900.0f;
	MissingPosition.DragThreshold = 6.0f;
	MissingPosition.SourceIsNativeHost = true;
	PF_CHECK(Tests, !Interaction.UpdateDrag(MissingPosition));
	MissingPosition.LeftButtonDown = false;
	MissingPosition.LeftButtonReleased = true;
	MissingPosition.PointerPosition = { -100001.0f, -100001.0f };
	const auto LastValidRelease = Interaction.UpdateDrag(MissingPosition);
	PF_CHECK(Tests, LastValidRelease && LastValidRelease->Action == DocumentTabReleaseAction::CreateNativeHost);
	PF_CHECK(Tests, LastValidRelease && LastValidRelease->ReleasePosition.X == 80.0f && LastValidRelease->ReleasePosition.Y == 40.0f);

	Interaction.BeginFrame();
	Interaction.BeginDrag(Level, DocumentTabPointerPosition{ 20.0f, 20.0f }, { 20.0f, 20.0f });
	Frame = {};
	Frame.LeftButtonDown = true;
	Frame.PointerPosition = { 20.0f, 20.0f };
	Frame.DragThreshold = 6.0f;
	Frame.SourceIsMainHost = true;
	PF_CHECK(Tests, !Interaction.UpdateDrag(Frame));
	Frame.LeftButtonDown = false;
	Frame.LeftButtonReleased = true;
	Frame.PointerPosition = { 180.0f, 120.0f };
	const auto LevelRelease = Interaction.UpdateDrag(Frame);
	PF_CHECK(Tests, LevelRelease && LevelRelease->Action == DocumentTabReleaseAction::None);

	EditorDocumentTransferRequest FirstRequest{ Material, "main", 1, false, std::nullopt };
	PF_CHECK(Tests, Interaction.QueueTransfer(FirstRequest));
	PF_CHECK(Tests, !Interaction.QueueTransfer({ Material, "another-host", 0, false, std::nullopt }));
	Interaction.BeginDrag(Material, DocumentTabPointerPosition{ 20.0f, 20.0f }, { 20.0f, 20.0f });
	Frame = {};
	Frame.LeftButtonReleased = true;
	Frame.LeftButtonDown = false;
	Frame.PointerPosition = { 90.0f, 20.0f };
	Frame.HoveredViewportID = 10;
	Frame.HoveredRootWindowID = 100;
	Frame.HasReliableHoveredViewport = true;
	Frame.MaximumObservedDistanceSquared = 4900.0f;
	Frame.DragThreshold = 6.0f;
	Frame.SourceIsMainHost = true;
	const auto DuplicateRelease = Interaction.UpdateDrag(Frame);
	PF_CHECK(Tests, DuplicateRelease && DuplicateRelease->Action == DocumentTabReleaseAction::None);
	const auto TakenRequest = Interaction.TakePendingTransfer();
	PF_CHECK(Tests, TakenRequest && TakenRequest->Identity == Material && TakenRequest->DestinationHostID == "main" &&
				TakenRequest->DestinationIndex == 1);
	PF_CHECK(Tests, !Interaction.TakePendingTransfer());

	Interaction.BeginFrame();
	Interaction.BeginDrag(Material, DocumentTabPointerPosition{ 20.0f, 20.0f }, { 20.0f, 20.0f });
	Interaction.CancelDragUntilNewPress();
	PF_CHECK(Tests, !Interaction.ActiveDragIdentity() && Interaction.IsCancelledUntilNewPress());
	Interaction.BeginFrame(false);
	Interaction.BeginDrag(Material, DocumentTabPointerPosition{ 20.0f, 20.0f }, { 80.0f, 20.0f });
	PF_CHECK(Tests, !Interaction.ActiveDragIdentity() && Interaction.IsCancelledUntilNewPress());
	PF_CHECK(Tests, ShouldCaptureDocumentTabDragInput(false, true, false, true, 4900.0f, 6.0f));
	Interaction.BeginFrame(true);
	PF_CHECK(Tests, !Interaction.IsCancelledUntilNewPress());
	Interaction.BeginDrag(Material, DocumentTabPointerPosition{ 20.0f, 20.0f }, { 80.0f, 20.0f });
	PF_CHECK(Tests, Interaction.ActiveDragIdentity() == Material);
	Interaction.CancelDrag();
	PF_CHECK(Tests, !Interaction.ActiveDragIdentity() && !Interaction.TakePendingTransfer());
}
