#include "TestSupport.h"

#include "Editor/EditorDocumentTabInteraction.h"

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
	Interaction.RegisterDropTarget({ "main", 10, { 10.0f, 10.0f }, { 110.0f, 50.0f }, 0, 1 });

	const auto BeforeMidpoint = Interaction.FindDropTarget({ 30.0f, 20.0f }, 10);
	const auto AfterMidpoint = Interaction.FindDropTarget({ 90.0f, 20.0f }, 10);
	PF_CHECK(Tests, BeforeMidpoint && BeforeMidpoint->HostID == "main" && BeforeMidpoint->Index == 0);
	PF_CHECK(Tests, AfterMidpoint && AfterMidpoint->HostID == "main" && AfterMidpoint->Index == 1);
	PF_CHECK(Tests, !Interaction.FindDropTarget({ 90.0f, 20.0f }, 11));
	PF_CHECK(Tests, !Interaction.FindDropTarget({ 110.0f, 20.0f }, 10));

	Interaction.BeginDrag(Material, DocumentTabPointerPosition{ 20.0f, 20.0f }, { 20.0f, 20.0f });
	EditorDocumentTabDragFrame DragFrame;
	DragFrame.LeftButtonDown = true;
	DragFrame.PointerPosition = { 90.0f, 20.0f };
	DragFrame.HoveredViewportID = 10;
	DragFrame.DragThreshold = 6.0f;
	DragFrame.SourceIsMainHost = true;
	PF_CHECK(Tests, !Interaction.UpdateDrag(DragFrame));
	PF_CHECK(Tests, Interaction.ActiveDragIdentity() == Material);

	Interaction.BeginFrame();
	PF_CHECK(Tests, !Interaction.FindDropTarget({ 90.0f, 20.0f }, 10));
	Interaction.RegisterDropTarget({ "main", 10, { 10.0f, 10.0f }, { 110.0f, 50.0f }, 0, 1 });
	DragFrame.LeftButtonDown = false;
	DragFrame.LeftButtonReleased = true;
	const auto StripRelease = Interaction.UpdateDrag(DragFrame);
	PF_CHECK(Tests, StripRelease && StripRelease->Identity == Material);
	PF_CHECK(Tests, StripRelease && StripRelease->Action == DocumentTabReleaseAction::TransferToStrip);
	PF_CHECK(Tests, StripRelease && StripRelease->DropLocation && StripRelease->DropLocation->HostID == "main" &&
						StripRelease->DropLocation->Index == 1);
	PF_CHECK(Tests, !Interaction.ActiveDragIdentity());

	Interaction.BeginFrame();
	Interaction.BeginDrag(Material, DocumentTabPointerPosition{ 20.0f, 20.0f }, { 20.0f, 20.0f });
	DragFrame.PointerPosition = { 180.0f, 120.0f };
	DragFrame.HoveredViewportID = 10;
	DragFrame.LeftButtonReleased = true;
	DragFrame.LeftButtonDown = false;
	const auto EditorBodyRelease = Interaction.UpdateDrag(DragFrame);
	PF_CHECK(Tests, EditorBodyRelease && EditorBodyRelease->Action == DocumentTabReleaseAction::CreateNativeHost);
	PF_CHECK(Tests, EditorBodyRelease && !EditorBodyRelease->DropLocation);

	Interaction.BeginFrame();
	Interaction.BeginDrag(Material, DocumentTabPointerPosition{ 20.0f, 20.0f }, { 20.0f, 20.0f });
	DragFrame = {};
	DragFrame.LeftButtonDown = true;
	DragFrame.PointerPosition = { 20.0f, 20.0f };
	DragFrame.HoveredViewportID = 10;
	DragFrame.DragThreshold = 6.0f;
	DragFrame.SourceIsNativeHost = true;
	PF_CHECK(Tests, !Interaction.UpdateDrag(DragFrame));
	DragFrame.LeftButtonDown = false;
	DragFrame.LeftButtonReleased = true;
	DragFrame.PointerPosition = { 180.0f, 120.0f };
	DragFrame.ReleaseOverMainEditorViewport = true;
	const auto UtilityPanelRelease = Interaction.UpdateDrag(DragFrame);
	PF_CHECK(Tests, UtilityPanelRelease && UtilityPanelRelease->Action == DocumentTabReleaseAction::None);

	EditorDocumentTransferRequest FirstRequest{ Material, "main", 1, false, std::nullopt };
	PF_CHECK(Tests, Interaction.QueueTransfer(FirstRequest));
	PF_CHECK(Tests, !Interaction.QueueTransfer({ Material, "another-host", 0, false, std::nullopt }));
	Interaction.BeginDrag(Material, DocumentTabPointerPosition{ 20.0f, 20.0f }, { 20.0f, 20.0f });
	DragFrame.PointerPosition = { 90.0f, 20.0f };
	DragFrame.LeftButtonReleased = true;
	DragFrame.HoveredViewportID = 10;
	const auto DuplicateRelease = Interaction.UpdateDrag(DragFrame);
	PF_CHECK(Tests, DuplicateRelease && DuplicateRelease->Action == DocumentTabReleaseAction::None);
	const auto TakenRequest = Interaction.TakePendingTransfer();
	PF_CHECK(Tests, TakenRequest && TakenRequest->Identity == Material && TakenRequest->DestinationHostID == "main" &&
						TakenRequest->DestinationIndex == 1);
	PF_CHECK(Tests, !Interaction.TakePendingTransfer());

	Interaction.BeginFrame();
	Interaction.BeginDrag(Material, DocumentTabPointerPosition{ 20.0f, 20.0f }, { 20.0f, 20.0f });
	DragFrame = {};
	DragFrame.LeftButtonDown = true;
	DragFrame.PointerPosition = { 80.0f, 40.0f };
	DragFrame.HoveredViewportID = 0;
	DragFrame.DragThreshold = 6.0f;
	DragFrame.SourceIsNativeHost = true;
	PF_CHECK(Tests, !Interaction.UpdateDrag(DragFrame));
	DragFrame.LeftButtonDown = false;
	DragFrame.LeftButtonReleased = true;
	DragFrame.PointerPosition = { -100001.0f, -100001.0f };
	const auto TearOffRelease = Interaction.UpdateDrag(DragFrame);
	PF_CHECK(Tests, TearOffRelease && TearOffRelease->Action == DocumentTabReleaseAction::CreateNativeHost);
	PF_CHECK(Tests, TearOffRelease && TearOffRelease->ReleasePosition.X == 80.0f && TearOffRelease->ReleasePosition.Y == 40.0f);

	Interaction.BeginDrag(Level, DocumentTabPointerPosition{ 20.0f, 20.0f }, { 20.0f, 20.0f });
	DragFrame.PointerPosition = { 80.0f, 40.0f };
	DragFrame.HoveredViewportID = 0;
	DragFrame.SourceIsNativeHost = true;
	const auto LevelRelease = Interaction.UpdateDrag(DragFrame);
	PF_CHECK(Tests, LevelRelease && LevelRelease->Action == DocumentTabReleaseAction::None);

	Interaction.BeginDrag(Material, DocumentTabPointerPosition{ 20.0f, 20.0f }, { 20.0f, 20.0f });
	DragFrame = {};
	DragFrame.PointerPosition = { 30.0f, 20.0f };
	PF_CHECK(Tests, !Interaction.UpdateDrag(DragFrame));
	PF_CHECK(Tests, !Interaction.ActiveDragIdentity());
	DragFrame.LeftButtonReleased = true;
	PF_CHECK(Tests, !Interaction.UpdateDrag(DragFrame));

	Interaction.BeginFrame();
	Interaction.RegisterDropTarget({ "main", 10, { 10.0f, 10.0f }, { 110.0f, 50.0f }, 0, 1 });
	Interaction.RegisterDropTarget({ "other", 10, { 10.0f, 10.0f }, { 110.0f, 50.0f }, 0, 1 });
	PF_CHECK(Tests, !Interaction.FindDropTarget({ 30.0f, 20.0f }, 10));
}
