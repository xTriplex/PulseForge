#include "TestEditorSupport.h"
#include "Editor/EditorDebugAcceptanceCoordinators.h"

namespace
{
	using namespace PulseForgeEditor::Detail;
	using PulseForgeEditor::EditorDocumentIdentity;
	using PulseForgeEditor::EditorDocumentKind;

	DocumentCloseUiSessionObservation InitialFixtureState()
	{
		DocumentCloseUiSessionObservation State;
		State.Open = true;
		State.HostSame = true;
		State.HostExists = true;
		State.Dirty = true;
		State.Value = 20;
		State.Saved = 10;
		return State;
	}

	void TestCoordinatorRejectsWrongIdentityAndInvalidTransition(TestRunner& Tests)
	{
		std::string ApplicationError;
		DocumentCloseAcceptanceCoordinator Coordinator(
			"cancel-save", [&ApplicationError](std::string Message) { ApplicationError = std::move(Message); });
		EditorDocumentIdentity Identity;
		Coordinator.SetIdentity(Identity);
		Coordinator.SetHost("fixture-host");
		Coordinator.FixtureReady(InitialFixtureState(), 1, 1, 1);

		EditorDocumentIdentity OtherIdentity;
		OtherIdentity.Kind = EditorDocumentKind::Level;
		PF_CHECK(Tests, Coordinator.IsFixtureIdentity(Identity) && !Coordinator.IsFixtureIdentity(OtherIdentity));
		PF_CHECK(Tests, Coordinator.Phase() == DocumentCloseUiPhase::AwaitingTextEntry);
		PF_CHECK(Tests, !Coordinator.BeginCloseRequest("fixture-host", false, {}));
		PF_CHECK(Tests, Coordinator.Phase() == DocumentCloseUiPhase::Failed);
		PF_CHECK(Tests, ApplicationError.find("unexpected test phase") != std::string::npos);
	}

	void TestSaveFailureAndClosePreparationRetryStates(TestRunner& Tests)
	{
		DocumentCloseAcceptanceCoordinator Coordinator("close-retry", [](std::string) {});
		Coordinator.SetIdentity(EditorDocumentIdentity{});
		Coordinator.SetHost("fixture-host");
		Coordinator.FixtureReady(InitialFixtureState(), 1, 1, 1);
		DocumentCloseUiTextObservation Text;
		Text.HostID = "fixture-host";
		Text.Clicked = true;
		Text.Active = true;
		Text.Changed = true;
		Text.Value = 42;
		Text.Session = InitialFixtureState();
		Text.Session.Value = 42;
		Coordinator.ReportTextField(Text);
		PF_CHECK(Tests, Coordinator.Phase() == DocumentCloseUiPhase::AwaitingInitialClose);
		PF_CHECK(Tests, Coordinator.BeginCloseRequest("fixture-host", false, {}));
		PF_CHECK(Tests, Coordinator.ExpectedChoice() == "save");

		DocumentCloseUiSessionObservation SavedButOpen;
		SavedButOpen.Pending = true;
		SavedButOpen.Open = true;
		SavedButOpen.HostSame = true;
		SavedButOpen.HostExists = true;
		SavedButOpen.Value = 42;
		SavedButOpen.Saved = 42;
		SavedButOpen.Error = "The Debug fixture is configured to reject close.";
		Coordinator.ReportAction("save", false, SavedButOpen);
		PF_CHECK(Tests, Coordinator.Phase() == DocumentCloseUiPhase::AwaitingCloseRetryCancel);
		PF_CHECK(Tests, SavedButOpen.Pending && SavedButOpen.Open && SavedButOpen.HostSame && !SavedButOpen.Dirty &&
							SavedButOpen.Value == 42 && SavedButOpen.Saved == 42);
		PF_CHECK(Tests, Coordinator.ExpectedChoice() == "cancel");

		SavedButOpen.Pending = false;
		SavedButOpen.Error.clear();
		Coordinator.ReportAction("cancel", true, SavedButOpen);
		PF_CHECK(Tests, Coordinator.Phase() == DocumentCloseUiPhase::AwaitingAllowClose);
		Coordinator.ReportAllowClose("fixture-host", 10, 11, true, SavedButOpen);
		PF_CHECK(Tests, Coordinator.Phase() == DocumentCloseUiPhase::AwaitingFinalClose);
		PF_CHECK(Tests, !Coordinator.Complete());
		Coordinator.ReportCleanClose(true, true, 42);
		PF_CHECK(Tests, Coordinator.Complete());
	}

	void TestSaveRetryKeepsDirtySessionUntilRetrySucceeds(TestRunner& Tests)
	{
		DocumentCloseAcceptanceCoordinator Coordinator("save-retry", [](std::string) {});
		Coordinator.SetIdentity(EditorDocumentIdentity{});
		Coordinator.SetHost("fixture-host");
		Coordinator.FixtureReady(InitialFixtureState(), 1, 1, 1);

		DocumentCloseUiTextObservation Text;
		Text.HostID = "fixture-host";
		Text.Clicked = true;
		Text.Active = true;
		Text.Changed = true;
		Text.Value = 42;
		Text.Session = InitialFixtureState();
		Text.Session.Value = 42;
		Coordinator.ReportTextField(Text);
		PF_CHECK(Tests, Coordinator.BeginCloseRequest("fixture-host", false, {}));

		auto FailedSave = InitialFixtureState();
		FailedSave.Pending = true;
		FailedSave.Value = 42;
		FailedSave.Error = "The document could not be saved; it remains open.";
		Coordinator.ReportAction("save", false, FailedSave);
		PF_CHECK(Tests, Coordinator.Phase() == DocumentCloseUiPhase::AwaitingSaveRetry);
		PF_CHECK(Tests, Coordinator.ExpectedChoice() == "save");
		PF_CHECK(Tests, FailedSave.Pending && FailedSave.Open && FailedSave.HostSame && FailedSave.HostExists && FailedSave.Dirty &&
							FailedSave.Value == 42 && FailedSave.Saved == 10);

		DocumentCloseUiSessionObservation SavedAndClosed;
		SavedAndClosed.Value = 42;
		SavedAndClosed.Saved = 42;
		Coordinator.ReportAction("save", true, SavedAndClosed);
		PF_CHECK(Tests, Coordinator.Complete());
	}

	void TestDetachedSceneViewportSmokeRequiresNativeOwnerAndCameraMotion(TestRunner& Tests)
	{
		SceneViewportNativeSmokeCoordinator Coordinator(true);
		SceneViewportNativeSmokeObservation Observation;
		Observation.MainViewportID = 1;
		Observation.OwnerViewportID = 1;
		Observation.OwnerWindow = 0x1000;
		Observation.SceneImageReady = true;
		Observation.CameraForward = { 0.0f, 0.0f, -1.0f };
		Observation.TitleCenter = { 150.0f, 50.0f };
		PF_CHECK(Tests, !Coordinator.Observe(Observation));
		Observation.DockTabReady = true;
		Observation.DockTabRect = { 100.0f, 30.0f, 200.0f, 70.0f };
		Observation.ImageMinimum = { 100.0f, 100.0f };
		Observation.ImageMaximum = { 700.0f, 500.0f };
		Observation.MainWindowRect = { 20.0f, 30.0f, 820.0f, 620.0f };
		Observation.TearOffPoint = { 860.0f, 340.0f };

		const auto Ready = Coordinator.Observe(Observation);
		PF_CHECK(Tests, Ready && Ready->find("=READY;") != std::string::npos);
		PF_CHECK(Tests, !Coordinator.Complete());

		Observation.OwnerViewportID = 2;
		Observation.OwnerWindow = 0x2000;
		Observation.OwnerNativePosition = { 300.0f, 100.0f };
		Observation.OwnerNativeSize = { 820.0f, 620.0f };
		Observation.ImageMinimum = { 400.0f, 200.0f };
		Observation.ImageMaximum = { 700.0f, 500.0f };
		Observation.OwnerFocused = true;
		Observation.CameraPosition = { 1.0f, 2.0f, 3.0f };
		Observation.CameraMoveSpeed = 6.0f;
		Observation.UtilityDropTargetReady = true;
		Observation.UtilityDropTargetRect = { 600.0f, 100.0f, 800.0f, 600.0f };
		Observation.UtilityDropTargetPanel = "Inspector";
		const auto Detached = Coordinator.Observe(Observation);
		PF_CHECK(Tests, Detached && Detached->find("=DETACHED;") != std::string::npos);
		PF_CHECK(Tests, !Coordinator.Complete());
		const auto DetachedState = Coordinator.ObserveState(Observation);
		PF_CHECK(Tests, DetachedState && DetachedState->find("camera_position=1.0000,2.0000,3.0000") != std::string::npos &&
			DetachedState->find("camera_speed=6.0000") != std::string::npos &&
			DetachedState->find("utility_drop_target=main_utility_panel_dock_node") != std::string::npos &&
			DetachedState->find("utility_drop_panel=Inspector") != std::string::npos &&
			DetachedState->find("utility_drop_rect=600.0000,100.0000,800.0000,600.0000") != std::string::npos);
		PF_CHECK(Tests, !Coordinator.ObserveInput(Observation));
		Observation.OwnerNativePosition = { 700.0f, 100.0f };
		Observation.OwnerNativeSize = { 500.0f, 400.0f };
		Observation.ImageMinimum = { 720.0f, 150.0f };
		Observation.ImageMaximum = { 980.0f, 400.0f };
		Observation.ImGuiTime = 0.1;
		PF_CHECK(Tests, !Coordinator.ObserveInput(Observation));
		Observation.ImGuiTime = 0.2;
		PF_CHECK(Tests, !Coordinator.ObserveInput(Observation));
		Observation.ImGuiTime = 0.4;
		const auto CameraReady = Coordinator.ObserveInput(Observation);
		PF_CHECK(Tests, CameraReady && CameraReady->find("=READY_FOR_CAMERA_INPUT;") != std::string::npos);
		PF_CHECK(Tests, CameraReady && CameraReady->find("sample=1") != std::string::npos);
		Observation.ImGuiTime = 1.2;
		const auto CameraReadyAgain = Coordinator.ObserveInput(Observation);
		PF_CHECK(Tests, CameraReadyAgain && CameraReadyAgain->find("sample=2") != std::string::npos);
		Observation.ImageMinimum = { 720.0f, 150.0f };
		Observation.ImGuiMousePosition = { 730.0f, 220.0f };
		Observation.RightButtonDown = true;
		Observation.RightButtonClicked = true;
		Observation.ImageHovered = true;
		Observation.ImGuiTime = 1.3;
		const auto Input = Coordinator.ObserveInput(Observation);
		PF_CHECK(Tests, Input && Input->find("event=clicked") != std::string::npos &&
			Input->find("native_rect=700.0,100.0,500.0,400.0") != std::string::npos);

		Observation.CameraCaptureActive = true;
		const auto Captured = Coordinator.Observe(Observation);
		PF_CHECK(Tests, Captured && Captured->find("=CAPTURED;") != std::string::npos);
		PF_CHECK(Tests, !Coordinator.Complete());

		Observation.CameraCaptureActive = false;
		Observation.OwnerFocused = false;
		PF_CHECK(Tests, !Coordinator.Observe(Observation));
		Observation.CameraForward = { 0.1f, 0.0f, -0.995f };
		PF_CHECK(Tests, !Coordinator.Observe(Observation));
		Observation.ImGuiTime = 0.5;
		const auto FocusLossState = Coordinator.ObserveState(Observation);
		PF_CHECK(Tests, FocusLossState && FocusLossState->find("focused=0; capture=0") != std::string::npos);
		Observation.OwnerFocused = true;
		Observation.RightButtonDown = false;
		Observation.RightButtonClicked = false;
		Observation.RightButtonReleased = true;
		const auto NormalRelease = Coordinator.ObserveInput(Observation);
		PF_CHECK(Tests, NormalRelease && NormalRelease->find("event=released") != std::string::npos);
		const auto Passed = Coordinator.Observe(Observation);
		PF_CHECK(Tests, Passed && Passed->find("=CAMERA_DRAG_PASS;") != std::string::npos);
		PF_CHECK(Tests, !Coordinator.Complete());
		PF_CHECK(Tests, !Coordinator.Observe(Observation));
		Observation.OwnerViewportID = Observation.MainViewportID;
		Observation.OwnerWindow = 0x2000;
		PF_CHECK(Tests, !Coordinator.Observe(Observation));
		Observation.OwnerWindow = 0x1001;
		PF_CHECK(Tests, !Coordinator.Observe(Observation));
		Observation.OwnerWindow = 0x1000;
		Observation.UtilityDockingDomainValid = true;
		Observation.UtilityDockNodeID = 42;
		Observation.DragTargetKind = SceneViewportDragTargetKind::DockTab;
		const auto Reattached = Coordinator.Observe(Observation);
		PF_CHECK(Tests, Reattached && Reattached->find("=REATTACHED;") != std::string::npos);
		PF_CHECK(Tests, Reattached && Reattached->find("camera_position=1.0000,2.0000,3.0000") != std::string::npos);
		PF_CHECK(Tests, Reattached && Reattached->find("camera_position=", Reattached->find("camera_position=") + 1) ==
			std::string::npos);
		PF_CHECK(Tests, !Coordinator.Complete());
		Observation.OwnerFocused = true;
		Observation.ViewportKeyboardFocused = true;
		Observation.CameraPosition = { 1.0f, 2.0f, 3.2f };
		const auto ReattachedInput = Coordinator.Observe(Observation);
		PF_CHECK(Tests, ReattachedInput && ReattachedInput->find("=REATTACHED_CAMERA_INPUT;") != std::string::npos);
		PF_CHECK(Tests, Coordinator.Complete());
		Observation.ImGuiTime = 300.0;
		PF_CHECK(Tests, !Coordinator.ObserveState(Observation));
	}

	void TestSceneViewportObservationsRemainLivePastLegacyLimit(TestRunner& Tests)
	{
		SceneViewportNativeSmokeCoordinator Coordinator(true);
		SceneViewportNativeSmokeObservation Observation;
		Observation.MainViewportID = 1;
		Observation.OwnerViewportID = 1;
		Observation.OwnerWindow = 0x1000;
		Observation.SceneImageReady = true;
		Observation.DockTabReady = true;
		Observation.ImGuiTime = 0.0;
		(void)Coordinator.Observe(Observation);

		Observation.OwnerViewportID = 2;
		Observation.OwnerWindow = 0x2000;
		Observation.OwnerFocused = true;
		Observation.ImGuiMousePosition = { 101.0f, 202.0f };
		Observation.ImGuiTime = 0.1;
		PF_CHECK(Tests, Coordinator.Observe(Observation).has_value());
		const auto InitialState = Coordinator.ObserveState(Observation);
		PF_CHECK(Tests, InitialState && InitialState->find("sequence=1") != std::string::npos &&
			InitialState->find("imgui_mouse=101.0000,202.0000") != std::string::npos);

		std::string LastState;
		for (int Index = 1; Index <= 105; ++Index)
		{
			Observation.ImGuiTime = 0.1 + static_cast<double>(Index) * 0.2;
			if (auto State = Coordinator.ObserveState(Observation)) LastState = *State;
		}
		PF_CHECK(Tests, !LastState.empty());

		const auto Sequence = [](const std::string& State)
		{
			const size_t Start = State.find("sequence=");
			if (Start == std::string::npos) return uint64_t{ 0 };
			return static_cast<uint64_t>(std::stoull(State.substr(Start + 9)));
		};
		const uint64_t LastSequence = Sequence(LastState);
		PF_CHECK(Tests, LastSequence > 1);

		Observation.OwnerFocused = false;
		Observation.ViewportKeyboardFocused = false;
		Observation.ImGuiTime += 0.01;
		const auto FocusChanged = Coordinator.ObserveState(Observation);
		PF_CHECK(Tests, FocusChanged && FocusChanged->find("focused=0") != std::string::npos &&
			FocusChanged->find("keyboard_focused=0") != std::string::npos && Sequence(*FocusChanged) > LastSequence);

		Observation.OwnerFocused = true;
		Observation.CameraCaptureActive = true;
		Observation.ImGuiTime += 0.01;
		PF_CHECK(Tests, Coordinator.Observe(Observation).has_value());
		Observation.RightButtonDown = true;
		Observation.RightButtonClicked = true;
		(void)Coordinator.ObserveInput(Observation);
		Observation.RightButtonDown = false;
		Observation.RightButtonClicked = false;
		Observation.RightButtonReleased = true;
		Observation.CameraCaptureActive = false;
		Observation.CameraForward = { 0.1f, 0.0f, -0.995f };
		Observation.ImGuiTime += 0.1;
		(void)Coordinator.ObserveInput(Observation);
		const auto Released = Coordinator.Observe(Observation);
		PF_CHECK(Tests, Released && Released->find("=CAMERA_DRAG_PASS;") != std::string::npos);
		Observation.OwnerViewportID = Observation.MainViewportID;
		Observation.OwnerWindow = 0x1000;
		Observation.ImGuiTime += 0.1;
		PF_CHECK(Tests, !Coordinator.Observe(Observation));
		Observation.UtilityDockingDomainValid = true;
		Observation.DragTargetKind = SceneViewportDragTargetKind::FloatingTitleBar;
		PF_CHECK(Tests, !Coordinator.Observe(Observation));
		Observation.UtilityDockNodeID = 42;
		Observation.DragTargetKind = SceneViewportDragTargetKind::DockTab;
		Observation.ImGuiTime += 0.1;
		const auto Reattached = Coordinator.Observe(Observation);
		PF_CHECK(Tests, Reattached && Reattached->find("=REATTACHED;") != std::string::npos);
		const auto ReattachedState = Coordinator.ObserveState(Observation);
		PF_CHECK(Tests, ReattachedState && Sequence(*ReattachedState) > LastSequence);
		PF_CHECK(Tests, !Coordinator.Complete());
		Observation.ViewportKeyboardFocused = true;
		Observation.CameraPosition = { 0.0f, 0.0f, 0.1f };
		Observation.ImGuiTime += 0.01;
		PF_CHECK(Tests, Coordinator.Observe(Observation).has_value());
		PF_CHECK(Tests, Coordinator.Complete());

		SceneViewportNativeSmokeCoordinator ExpiringCoordinator(true);
		SceneViewportNativeSmokeObservation Expiring = Observation;
		Expiring.OwnerViewportID = 1;
		Expiring.OwnerWindow = 0x3000;
		Expiring.MainViewportID = 1;
		Expiring.ImGuiTime = 0.0;
		(void)ExpiringCoordinator.Observe(Expiring);
		Expiring.OwnerViewportID = 2;
		Expiring.OwnerWindow = 0x4000;
		Expiring.ImGuiTime = 0.1;
		(void)ExpiringCoordinator.Observe(Expiring);
		Expiring.ImGuiTime = 270.0;
		Expiring.OwnerWindow = 0;
		const auto Expired = ExpiringCoordinator.ObserveState(Expiring);
		PF_CHECK(Tests, Expired && Expired->find("=OBSERVATION_EXPIRED;") != std::string::npos);
		PF_CHECK(Tests, !ExpiringCoordinator.ObserveState(Expiring));
		PF_CHECK(Tests, !ExpiringCoordinator.Complete());
	}
} // namespace

void RunEditorDebugAcceptanceCoordinatorTests(TestRunner& Tests)
{
	TestCoordinatorRejectsWrongIdentityAndInvalidTransition(Tests);
	TestSaveRetryKeepsDirtySessionUntilRetrySucceeds(Tests);
	TestSaveFailureAndClosePreparationRetryStates(Tests);
	TestDetachedSceneViewportSmokeRequiresNativeOwnerAndCameraMotion(Tests);
	TestSceneViewportObservationsRemainLivePastLegacyLimit(Tests);
}
