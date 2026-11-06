#include "TestSupport.h"
#include "Editor/EditorDocumentTabInteraction.h"

#include <utility>

void RunEditorDocumentTabInteractionTests(TestRunner& Tests)
{
	using namespace PulseForge;
	using namespace PulseForgeEditor;

	const auto Project = UUID::Parse("30000000-0000-4000-8000-000000000001");
	PF_CHECK(Tests, Project.has_value());
	if (!Project) return;
	const EditorDocumentIdentity Material{ *Project, UUID{}, EditorDocumentKind::Material };
	const EditorDocumentIdentity Level{ *Project, UUID{}, EditorDocumentKind::Level };
	EditorDocumentTabInteraction Interaction;

	const auto Register = [&Interaction](std::string Host, uint32_t Viewport, uint32_t Root,
		DocumentTabPointerPosition Minimum, DocumentTabPointerPosition Maximum, size_t Before, size_t After,
		bool CanReceive = true, DocumentTabPointerPosition ClipMinimum = { 0.0f, 0.0f },
		DocumentTabPointerPosition ClipMaximum = { 2000.0f, 2000.0f })
	{
		Interaction.RegisterDropTarget({ std::move(Host), Viewport, Root, Minimum, Maximum, ClipMinimum, ClipMaximum, Before, After,
			CanReceive });
	};
	const auto StartDrag = [&Interaction](const EditorDocumentIdentity& Identity)
	{
		Interaction.BeginDrag(Identity, DocumentTabPointerPosition{ 20.0f, 20.0f }, { 20.0f, 20.0f });
	};
	const auto MakeFrame = [](DocumentTabPointerPosition Position, uint32_t Viewport, uint32_t Root,
		bool LeftDown = true, bool Reliable = true)
	{
		EditorDocumentTabDragFrame Frame;
		Frame.LeftButtonDown = LeftDown;
		Frame.HasReliableHoveredViewport = Reliable;
		Frame.PointerPosition = Position;
		Frame.HoveredViewportID = Viewport;
		Frame.HoveredRootWindowID = Root;
		Frame.MaximumObservedDistanceSquared = 4900.0f;
		Frame.DragThreshold = 6.0f;
		Frame.SourceIsMainHost = true;
		return Frame;
	};

	PF_CHECK(Tests, ShouldSuppressTabPressAfterDrag(true, true));
	PF_CHECK(Tests, !ShouldSuppressTabPressAfterDrag(true, false));
	PF_CHECK(Tests, CanCreateNativeHostFromDocumentTabDrag(true, false));
	PF_CHECK(Tests, CanCreateNativeHostFromDocumentTabDrag(false, true));
	PF_CHECK(Tests, !CanCreateNativeHostFromDocumentTabDrag(false, false));
	PF_CHECK(Tests, ShouldCaptureDocumentTabDragInput(true, false, true, false, 40.0f, 6.0f));
	PF_CHECK(Tests, !ShouldCaptureDocumentTabDragInput(true, false, true, false, 20.0f, 6.0f));
	PF_CHECK(Tests, ShouldCaptureDocumentTabDragInput(false, true, false, true, 40.0f, 6.0f));

	// A captured gesture can resolve feedback without an ImGui drag payload. The same result
	// feeds release routing, so the cue cannot disagree with the transfer destination.
	Interaction.BeginFrame();
	Register("main", 10, 100, { 10.0f, 10.0f }, { 110.0f, 50.0f }, 2, 3);
	StartDrag(Material);
	auto Frame = MakeFrame({ 30.0f, 20.0f }, 10, 100);
	Frame.ImGuiPayloadActive = false;
	const auto BeforeMidpoint = Interaction.ResolveDropPreview(Frame);
	PF_CHECK(Tests, BeforeMidpoint && BeforeMidpoint->Identity == Material);
	PF_CHECK(Tests, BeforeMidpoint && BeforeMidpoint->Location.HostID == "main" && BeforeMidpoint->Location.Index == 2);
	PF_CHECK(Tests, BeforeMidpoint && BeforeMidpoint->InsertionX == 10.0f && !BeforeMidpoint->IsAppend);
	PF_CHECK(Tests, !Interaction.TakePendingTransfer());
	Frame.PointerPosition = { 90.0f, 20.0f };
	const auto AfterMidpoint = Interaction.ResolveDropPreview(Frame);
	PF_CHECK(Tests, AfterMidpoint && AfterMidpoint->Location.Index == 3 && AfterMidpoint->InsertionX == 110.0f);
	Frame.LeftButtonDown = false;
	Frame.LeftButtonReleased = true;
	const auto ReleasedToTab = Interaction.UpdateDrag(Frame);
	PF_CHECK(Tests, ReleasedToTab && ReleasedToTab->Action == DocumentTabReleaseAction::TransferToStrip);
	PF_CHECK(Tests, ReleasedToTab && ReleasedToTab->DropLocation && ReleasedToTab->DropLocation->HostID == AfterMidpoint->Location.HostID &&
			ReleasedToTab->DropLocation->Index == AfterMidpoint->Location.Index);
	PF_CHECK(Tests, !Interaction.ResolveDropPreview(Frame));

	// Compatible native strips use the same location calculation; the empty tail is an append cue.
	Interaction.BeginFrame();
	Register("native", 11, 200, { 10.0f, 10.0f }, { 110.0f, 50.0f }, 0, 1);
	Register("native", 11, 200, { 110.0f, 10.0f }, { 500.0f, 50.0f }, 4, 4);
	StartDrag(Material);
	Frame = MakeFrame({ 30.0f, 20.0f }, 11, 200);
	Frame.SourceIsMainHost = false;
	Frame.SourceIsNativeHost = true;
	const auto NativeTab = Interaction.ResolveDropPreview(Frame);
	PF_CHECK(Tests, NativeTab && NativeTab->Location.HostID == "native" && NativeTab->Location.Index == 0);
	Frame.PointerPosition = { 180.0f, 20.0f };
	const auto Append = Interaction.ResolveDropPreview(Frame);
	PF_CHECK(Tests, Append && Append->Location.HostID == "native" && Append->Location.Index == 4 && Append->IsAppend);
	PF_CHECK(Tests, Append && Append->InsertionX == 110.0f);
	Frame.LeftButtonDown = false;
	Frame.LeftButtonReleased = true;
	const auto ReleasedToAppend = Interaction.UpdateDrag(Frame);
	PF_CHECK(Tests, ReleasedToAppend && ReleasedToAppend->Action == DocumentTabReleaseAction::TransferToStrip);
	PF_CHECK(Tests, ReleasedToAppend && ReleasedToAppend->DropLocation && ReleasedToAppend->DropLocation->Index == Append->Location.Index);

	// Registration order cannot select an obscured strip; the actual viewport and root window win.
	Interaction.BeginFrame();
	Register("main", 10, 100, { 10.0f, 10.0f }, { 110.0f, 50.0f }, 0, 1);
	Register("native", 11, 200, { 10.0f, 10.0f }, { 110.0f, 50.0f }, 0, 1);
	StartDrag(Material);
	Frame = MakeFrame({ 30.0f, 20.0f }, 11, 200);
	const auto NativeFirst = Interaction.ResolveDropPreview(Frame);
	PF_CHECK(Tests, NativeFirst && NativeFirst->Location.HostID == "native");
	Interaction.BeginFrame();
	Register("native", 11, 200, { 10.0f, 10.0f }, { 110.0f, 50.0f }, 0, 1);
	Register("main", 10, 100, { 10.0f, 10.0f }, { 110.0f, 50.0f }, 0, 1);
	const auto MainRegisteredFirst = Interaction.ResolveDropPreview(Frame);
	PF_CHECK(Tests, MainRegisteredFirst && MainRegisteredFirst->Location.HostID == "native");
	Frame.HoveredViewportID = 10;
	Frame.HoveredRootWindowID = 100;
	const auto MainHovered = Interaction.ResolveDropPreview(Frame);
	PF_CHECK(Tests, MainHovered && MainHovered->Location.HostID == "main");
	Frame.HoveredRootWindowID = 201;
	PF_CHECK(Tests, !Interaction.ResolveDropPreview(Frame));
	Frame.HoveredViewportID = 0;
	PF_CHECK(Tests, !Interaction.ResolveDropPreview(Frame));
	Frame.HoveredViewportID = 10;
	Frame.HoveredRootWindowID = 100;
	Frame.HasReliableHoveredViewport = false;
	PF_CHECK(Tests, !Interaction.ResolveDropPreview(Frame));

	// Clipping affects both hit testing and the region drawn by the caller.
	Interaction.BeginFrame();
	Register("main", 10, 100, { 10.0f, 10.0f }, { 110.0f, 50.0f }, 0, 1, true, { 50.0f, 15.0f }, { 100.0f, 45.0f });
	Frame = MakeFrame({ 40.0f, 20.0f }, 10, 100);
	PF_CHECK(Tests, !Interaction.ResolveDropPreview(Frame));
	Frame.PointerPosition = { 60.0f, 20.0f };
	const auto Clipped = Interaction.ResolveDropPreview(Frame);
	PF_CHECK(Tests, Clipped && Clipped->Minimum.X == 50.0f && Clipped->Maximum.X == 100.0f);

	// An incompatible target, ambiguous overlap, or vanished target never advertises attachment.
	Interaction.BeginFrame();
	Register("incompatible", 12, 300, { 10.0f, 10.0f }, { 110.0f, 50.0f }, 0, 1, false);
	Frame = MakeFrame({ 30.0f, 20.0f }, 12, 300);
	const auto Incompatible = Interaction.ResolveDropPreview(Frame);
	PF_CHECK(Tests, !Incompatible);
	Frame.LeftButtonDown = false;
	Frame.LeftButtonReleased = true;
	const auto IncompatibleRelease = Interaction.UpdateDrag(Frame);
	PF_CHECK(Tests, IncompatibleRelease && IncompatibleRelease->Action == DocumentTabReleaseAction::CreateNativeHost);

	Interaction.BeginFrame();
	Register("first", 10, 100, { 10.0f, 10.0f }, { 70.0f, 50.0f }, 0, 1);
	Register("second", 10, 100, { 20.0f, 10.0f }, { 80.0f, 50.0f }, 0, 1);
	StartDrag(Material);
	Frame = MakeFrame({ 30.0f, 20.0f }, 10, 100);
	PF_CHECK(Tests, !Interaction.ResolveDropPreview(Frame));
	Interaction.BeginFrame();
	PF_CHECK(Tests, !Interaction.ResolveDropPreview(Frame));
	Frame.LeftButtonDown = false;
	Frame.LeftButtonReleased = true;
	const auto VanishedTargetRelease = Interaction.UpdateDrag(Frame);
	PF_CHECK(Tests, VanishedTargetRelease && VanishedTargetRelease->Action == DocumentTabReleaseAction::CreateNativeHost);

	Interaction.BeginFrame();
	Register("main", 10, 100, { 10.0f, 10.0f }, { 110.0f, 50.0f }, 0, 1);
	StartDrag(Material);
	Frame = MakeFrame({ 30.0f, 20.0f }, 10, 100);
	Frame.SourceIsMainHost = false;
	Frame.SourceIsNativeHost = false;
	PF_CHECK(Tests, !Interaction.ResolveDropPreview(Frame));
	Frame.LeftButtonDown = false;
	Frame.LeftButtonReleased = true;
	const auto MissingSourceRelease = Interaction.UpdateDrag(Frame);
	PF_CHECK(Tests, MissingSourceRelease && MissingSourceRelease->Action == DocumentTabReleaseAction::None);

	Interaction.BeginFrame();
	StartDrag(Material);
	Frame = MakeFrame({ 80.0f, 40.0f }, 0, 0);
	Frame.SourceIsMainHost = false;
	Frame.SourceIsNativeHost = true;
	PF_CHECK(Tests, !Interaction.UpdateDrag(Frame));
	Frame.PointerPosition = { -100001.0f, -100001.0f };
	Frame.LeftButtonDown = false;
	Frame.LeftButtonReleased = true;
	const auto LastValidRelease = Interaction.UpdateDrag(Frame);
	PF_CHECK(Tests, LastValidRelease && LastValidRelease->Action == DocumentTabReleaseAction::CreateNativeHost);
	PF_CHECK(Tests, LastValidRelease && LastValidRelease->ReleasePosition.X == 80.0f && LastValidRelease->ReleasePosition.Y == 40.0f);

	// Below threshold and Level never receive docking feedback; canceled input stays canceled until a fresh press.
	Interaction.BeginFrame();
	StartDrag(Material);
	Frame = MakeFrame({ 22.0f, 20.0f }, 10, 100);
	Frame.MaximumObservedDistanceSquared = 4.0f;
	Register("main", 10, 100, { 10.0f, 10.0f }, { 110.0f, 50.0f }, 0, 1);
	PF_CHECK(Tests, !Interaction.ResolveDropPreview(Frame));
	Frame.LeftButtonDown = false;
	Frame.LeftButtonReleased = true;
	const auto BelowThresholdRelease = Interaction.UpdateDrag(Frame);
	PF_CHECK(Tests, BelowThresholdRelease && BelowThresholdRelease->Action == DocumentTabReleaseAction::None);
	Interaction.BeginFrame();
	StartDrag(Material);
	Interaction.CancelDragUntilNewPress();
	PF_CHECK(Tests, !Interaction.ActiveDragIdentity() && Interaction.IsCancelledUntilNewPress());
	Interaction.BeginFrame(false);
	StartDrag(Material);
	PF_CHECK(Tests, !Interaction.ActiveDragIdentity());
	Interaction.BeginFrame(true);
	StartDrag(Material);
	PF_CHECK(Tests, Interaction.ActiveDragIdentity() == Material);
	Interaction.BeginFrame();
	Register("main", 10, 100, { 10.0f, 10.0f }, { 110.0f, 50.0f }, 0, 1);
	Frame = MakeFrame({ 30.0f, 20.0f }, 10, 100);
	PF_CHECK(Tests, Interaction.ResolveDropPreview(Frame));
	StartDrag(Level);
	PF_CHECK(Tests, !Interaction.ResolveDropPreview(Frame));
	Frame.LeftButtonDown = false;
	Frame.LeftButtonReleased = true;
	const auto LevelRelease = Interaction.UpdateDrag(Frame);
	PF_CHECK(Tests, LevelRelease && LevelRelease->Action == DocumentTabReleaseAction::None);

	Interaction.BeginFrame();
	StartDrag(Material);
	const EditorDocumentTransferRequest ExistingTransfer{ Material, "main", 1, false, std::nullopt };
	PF_CHECK(Tests, Interaction.QueueTransfer(ExistingTransfer));
	Frame = MakeFrame({ 90.0f, 20.0f }, 10, 100, false);
	Frame.LeftButtonReleased = true;
	Register("main", 10, 100, { 10.0f, 10.0f }, { 110.0f, 50.0f }, 0, 1);
	const auto DuplicateRelease = Interaction.UpdateDrag(Frame);
	PF_CHECK(Tests, DuplicateRelease && DuplicateRelease->Action == DocumentTabReleaseAction::None);
	const auto TakenRequest = Interaction.TakePendingTransfer();
	PF_CHECK(Tests, TakenRequest && TakenRequest->Identity == Material && TakenRequest->DestinationHostID == "main" &&
			TakenRequest->DestinationIndex == 1);
	PF_CHECK(Tests, !Interaction.TakePendingTransfer());
}
