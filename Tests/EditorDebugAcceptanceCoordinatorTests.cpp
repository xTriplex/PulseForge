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
} // namespace

void RunEditorDebugAcceptanceCoordinatorTests(TestRunner& Tests)
{
	TestCoordinatorRejectsWrongIdentityAndInvalidTransition(Tests);
	TestSaveRetryKeepsDirtySessionUntilRetrySucceeds(Tests);
	TestSaveFailureAndClosePreparationRetryStates(Tests);
}
