#include "TestEditorSupport.h"

namespace
{
	void TestEditorDocumentEditorRegistry(TestRunner& Tests)
	{
		using namespace PulseForge;
		using namespace PulseForgeEditor;
		struct Model final : EditorDocumentEditorState
		{
			int Value = 0;
			std::vector<int> Undo;
			std::vector<int> Redo;
			bool FailSave = false;
			bool FailClose = false;
			bool EditDuringSave = false;
		};

		const auto Project = UUID::Generate();
		const auto AssetA = UUID::Generate();
		const auto AssetB = UUID::Generate();
		PF_CHECK(Tests, Project && AssetA && AssetB);
		if (!Project || !AssetA || !AssetB) return;

		std::unordered_map<UUID, int, UUIDHash> SavedValues;
		EditorDocumentEditorRegistry Registry;
		EditorDocumentEditorAdapter Adapter;
		Adapter.Kind = EditorDocumentKind::Texture;
		Adapter.IsEligible = [](const EditorDocumentEditorContext& Context)
		{ return Context.Identity.Kind == EditorDocumentKind::Texture && Context.ProjectRelativePath.extension() == ".fixture"; };
		Adapter.LoadSession =
			[&SavedValues](
				const EditorDocumentEditorContext& Context) -> std::expected<std::unique_ptr<EditorDocumentEditorState>, std::string>
		{
			auto State = std::make_unique<Model>();
			if (const auto Saved = SavedValues.find(Context.Identity.Asset); Saved != SavedValues.end()) State->Value = Saved->second;
			return std::unique_ptr<EditorDocumentEditorState>(std::move(State));
		};
		Adapter.DrawWorkspace = [](EditorDocumentSession&) {};
		Adapter.Save = [&SavedValues](EditorDocumentSession& Session, uint64_t)
		{
			auto* State = dynamic_cast<Model*>(Session.EditorState.get());
			if (!State || State->FailSave) return false;
			SavedValues[Session.Identity.Asset] = State->Value;
			if (State->EditDuringSave)
			{
				State->EditDuringSave = false;
				State->Undo.push_back(State->Value);
				++State->Value;
				++Session.Revision;
			}
			return true;
		};
		Adapter.Discard = [&SavedValues](EditorDocumentSession& Session)
		{
			auto* State = dynamic_cast<Model*>(Session.EditorState.get());
			if (!State) return false;
			State->Value = SavedValues[Session.Identity.Asset];
			State->Undo.clear();
			State->Redo.clear();
			return true;
		};
		Adapter.FocusedCommand = [](EditorDocumentSession& Session, EditorDocumentCommand Command)
		{
			auto* State = dynamic_cast<Model*>(Session.EditorState.get());
			if (!State) return false;
			if (Command == EditorDocumentCommand::Undo && !State->Undo.empty())
			{
				State->Redo.push_back(State->Value);
				State->Value = State->Undo.back();
				State->Undo.pop_back();
				++Session.Revision;
				return true;
			}
			if (Command == EditorDocumentCommand::Redo && !State->Redo.empty())
			{
				State->Undo.push_back(State->Value);
				State->Value = State->Redo.back();
				State->Redo.pop_back();
				++Session.Revision;
				return true;
			}
			return false;
		};
		Adapter.AssetLocationChanged = [](EditorDocumentSession&, const std::filesystem::path&, std::string_view) { return true; };
		Adapter.PrepareClose = [](const EditorDocumentSession& Session) -> std::expected<void, std::string>
		{
			const auto* State = dynamic_cast<const Model*>(Session.EditorState.get());
			if (State && State->FailClose) return std::unexpected("close rejected by test fixture");
			return {};
		};
		size_t CloseCount = 0;
		Adapter.Close = [&CloseCount](EditorDocumentSession&) { ++CloseCount; };
		PF_CHECK(Tests, !Registry.IsRegistered(EditorDocumentKind::Texture));
		PF_CHECK(Tests, Registry.Register(Adapter).has_value());
		PF_CHECK(Tests, Registry.IsRegistered(EditorDocumentKind::Texture));
		PF_CHECK(Tests, !Registry.Register(Adapter));

		const EditorDocumentIdentity IdentityA{ *Project, *AssetA, EditorDocumentKind::Texture };
		const EditorDocumentIdentity IdentityB{ *Project, *AssetB, EditorDocumentKind::Texture };
		EditorDocumentEditorContext ContextA{ IdentityA, "Projects/Fixture", "Assets/A.fixture", "A.fixture", nullptr };
		EditorDocumentEditorContext ContextB{ IdentityB, "Projects/Fixture", "Assets/B.fixture", "B.fixture", nullptr };
		PF_CHECK(Tests, Registry.CanOpen(ContextA));
		ContextA.ProjectRelativePath = "Assets/A.png";
		PF_CHECK(Tests, !Registry.CanOpen(ContextA));
		ContextA.ProjectRelativePath = "Assets/A.fixture";

		EditorDocumentManager Manager;
		const auto OpenA =
			Manager.Open(IdentityA, ContextA.ProjectRelativePath, ContextA.DisplayName, EditorDocumentOpenPreference::MainDocumentStrip);
		const auto OpenB =
			Manager.Open(IdentityB, ContextB.ProjectRelativePath, ContextB.DisplayName, EditorDocumentOpenPreference::MainDocumentStrip);
		PF_CHECK(Tests, OpenA && OpenB);
		auto* SessionA = Manager.Find(IdentityA);
		auto* SessionB = Manager.Find(IdentityB);
		PF_CHECK(Tests, SessionA && SessionB && Registry.Load(*SessionA, ContextA) && Registry.Load(*SessionB, ContextB));
		auto* StateA = SessionA ? dynamic_cast<Model*>(SessionA->EditorState.get()) : nullptr;
		auto* StateB = SessionB ? dynamic_cast<Model*>(SessionB->EditorState.get()) : nullptr;
		PF_CHECK(Tests, StateA && StateB && StateA != StateB);
		if (!StateA || !StateB || !SessionA || !SessionB) return;
		StateA->Value = 11;
		StateA->Undo.push_back(0);
		PF_CHECK(Tests, Manager.MarkEdited(IdentityA));
		StateB->Value = 22;
		StateB->Undo.push_back(0);
		PF_CHECK(Tests, Manager.MarkEdited(IdentityB));
		StateA->FailSave = true;
		PF_CHECK(Tests, !Registry.Save(*SessionA) && SessionA->IsDirty());
		StateA->FailSave = false;
		StateA->EditDuringSave = true;
		PF_CHECK(Tests, Registry.Save(*SessionA) && SessionA->IsDirty() && SavedValues[*AssetA] == 11);
		PF_CHECK(Tests, Registry.Save(*SessionA) && !SessionA->IsDirty());
		StateA->Value = 99;
		PF_CHECK(Tests, Manager.MarkEdited(IdentityA));
		PF_CHECK(Tests, Registry.Discard(*SessionA) && StateA->Value == 12 && !SessionA->IsDirty());
		StateA->Undo.push_back(StateA->Value);
		StateA->Value = 0;
		PF_CHECK(Tests, Manager.MarkEdited(IdentityA));

		const auto NativeHost = Manager.CreateNativeHost(EditorDocumentKind::Texture);
		PF_CHECK(Tests, NativeHost.has_value());
		const EditorDocumentEditorState* OriginalStateAddress = SessionA->EditorState.get();
		if (NativeHost)
		{
			PF_CHECK(Tests, Manager.Transfer(IdentityA, *NativeHost, 0).has_value());
			PF_CHECK(Tests, Manager.Find(IdentityA)->EditorState.get() == OriginalStateAddress && StateA->Value == 0);
		}
		PF_CHECK(Tests, Manager.Activate(IdentityA));
		PF_CHECK(Tests, Registry.DispatchFocusedCommand(*SessionA, EditorDocumentCommand::Undo));
		PF_CHECK(Tests, StateA->Value == 12 && StateB->Value == 22);
		const uint64_t BeforeLocationChangeRevision = SessionA->Revision;
		PF_CHECK(Tests, Registry.UpdateAssetLocation(*SessionA, "Assets/Renamed/A.fixture", "Renamed.fixture"));
		PF_CHECK(Tests, SessionA->ProjectRelativePath == "Assets/Renamed/A.fixture" && SessionA->Revision == BeforeLocationChangeRevision);

		StateA->Value = 13;
		PF_CHECK(Tests, Manager.MarkEdited(IdentityA));
		StateA->FailClose = true;
		const std::array<EditorDocumentIdentity, 1> SingleCloseIdentity = { IdentityA };
		size_t DestructiveDiscardCount = 0;
		std::function<void()> PendingDiscardContinuation = [] {};
		std::vector<std::function<void()>> DiscardContinuations;
		const auto RejectedDirtyDiscard = ResolveDocumentCloseBatchWithChanges(
			Manager, Registry, SingleCloseIdentity, true,
			[&]() -> std::expected<void, std::string>
			{
				++DestructiveDiscardCount;
				return Registry.Discard(*SessionA) ? std::expected<void, std::string>{}
												   : std::unexpected("discard rejected by test fixture");
			},
			PendingDiscardContinuation,
			[&DiscardContinuations](std::function<void()> Action) { DiscardContinuations.push_back(std::move(Action)); });
		PF_CHECK(Tests, !RejectedDirtyDiscard && DestructiveDiscardCount == 0 && PendingDiscardContinuation);
		PF_CHECK(Tests, Manager.Find(IdentityA) == SessionA && StateA->Value == 13 && SessionA->IsDirty());
		PF_CHECK(Tests, DiscardContinuations.empty() && CloseCount == 0);
		const auto FailedPreparation = Registry.PrepareClose(*SessionA);
		PF_CHECK(Tests, !FailedPreparation && CloseCount == 0 && Manager.Find(IdentityA) == SessionA && StateA->Value == 13);
		StateA->FailClose = false;
		PF_CHECK(Tests, Registry.PrepareClose(*SessionA).has_value() && CloseCount == 0);
		PF_CHECK(Tests, Manager.Close(
							IdentityA, EditorDocumentCloseChoice::Cancel, [&Registry](const auto&, uint64_t) { return false; },
							[&Registry](const auto&) { return false; }) == EditorDocumentCloseStatus::Cancelled);
		PF_CHECK(Tests, Manager.Find(IdentityA) && Manager.Find(IdentityA)->EditorState.get() == OriginalStateAddress);

		EditorDocumentEditorRegistry RestartRegistry;
		PF_CHECK(Tests, RestartRegistry.Register(Adapter).has_value());
		EditorDocumentManager RestartManager;
		const auto RestoredOpen =
			RestartManager.Open(IdentityA, "Assets/Renamed/A.fixture", "Renamed.fixture", EditorDocumentOpenPreference::MainDocumentStrip);
		PF_CHECK(Tests, RestoredOpen.has_value());
		if (RestoredOpen)
		{
			auto* RestoredSession = RestartManager.Find(IdentityA);
			EditorDocumentEditorContext RestoredContext = ContextA;
			RestoredContext.ProjectRelativePath = "Assets/Renamed/A.fixture";
			PF_CHECK(Tests, RestoredSession && RestartRegistry.Load(*RestoredSession, RestoredContext));
			const auto* RestoredModel = RestoredSession ? dynamic_cast<const Model*>(RestoredSession->EditorState.get()) : nullptr;
			PF_CHECK(Tests, RestoredModel && RestoredModel->Value == 12);
		}

		PF_CHECK(Tests, Registry.Save(*SessionA));
		StateA->Value = 77;
		PF_CHECK(Tests, Manager.MarkEdited(IdentityA) && Registry.Save(*SessionA));
		StateA->Undo.push_back(StateA->Value);
		StateA->Value = 78;
		PF_CHECK(Tests, Manager.MarkEdited(IdentityA) && SessionA->IsDirty());
		PF_CHECK(Tests, Registry.Save(*SessionA) && !SessionA->IsDirty() && SavedValues[*AssetA] == 78);
		StateA->FailClose = true;
		size_t SingleCloseContinuationCount = 0;
		std::function<void()> PendingSingleCloseContinuation = [&SingleCloseContinuationCount] { ++SingleCloseContinuationCount; };
		std::vector<std::function<void()>> SingleCloseContinuations;
		const auto FailedSavedDirtyTabClose =
			ResolveDocumentCloseBatch(Manager, Registry, SingleCloseIdentity, PendingSingleCloseContinuation,
				[&SingleCloseContinuations](std::function<void()> Action) { SingleCloseContinuations.push_back(std::move(Action)); });
		PF_CHECK(Tests, !FailedSavedDirtyTabClose && PendingSingleCloseContinuation && SingleCloseContinuations.empty() &&
							SingleCloseContinuationCount == 0);
		PF_CHECK(Tests, Manager.Find(IdentityA) == SessionA && SessionA->EditorState.get() == OriginalStateAddress &&
							!SessionA->IsDirty() && StateA->Value == 78 && SavedValues[*AssetA] == 78);
		PendingSingleCloseContinuation = {};
		PF_CHECK(Tests, Manager.Find(IdentityA) == SessionA && StateA->Value == 78 && SingleCloseContinuationCount == 0);
		StateA->FailClose = false;
		PF_CHECK(Tests, Registry.Save(*SessionB));
		if (NativeHost)
		{
			const auto* Host = Manager.FindHost(*NativeHost);
			const size_t DestinationIndex = Host ? Host->Documents.size() : 0;
			PF_CHECK(Tests, Manager.Transfer(IdentityB, *NativeHost, DestinationIndex).has_value());
		}
		StateB->FailClose = true;
		const auto* SharedHostBeforeClose = Manager.FindHost(SessionA->HostID);
		const auto HostOrderBeforeClose = SharedHostBeforeClose ? SharedHostBeforeClose->Documents : std::vector<EditorDocumentIdentity>{};
		const auto ActiveBeforeClose =
			SharedHostBeforeClose ? SharedHostBeforeClose->ActiveDocument : std::optional<EditorDocumentIdentity>{};
		const std::array<EditorDocumentIdentity, 2> BatchIdentities = { IdentityA, IdentityB };
		auto RejectedBatch = EditorDocumentCloseBatch::Prepare(Manager, Registry, BatchIdentities);
		PF_CHECK(Tests, !RejectedBatch && CloseCount == 0);
		PF_CHECK(Tests, SharedHostBeforeClose && HostOrderBeforeClose.size() == 2 && SessionA->HostID == SessionB->HostID);
		PF_CHECK(Tests, SavedValues[*AssetA] == 78 && Manager.Find(IdentityA) == SessionA && !SessionA->IsDirty());
		PF_CHECK(Tests, Manager.Find(IdentityB) == SessionB && StateB->Value == 22);
		const auto* SharedHostAfterFailure = Manager.FindHost(SessionA->HostID);
		PF_CHECK(Tests, SharedHostAfterFailure && SharedHostAfterFailure->Documents == HostOrderBeforeClose &&
							SharedHostAfterFailure->ActiveDocument == ActiveBeforeClose);
		size_t ProjectSwitchContinuationCount = 0;
		size_t ShutdownContinuationCount = 0;
		std::vector<std::function<void()>> QueuedContinuations;
		std::function<void()> PendingProjectSwitchContinuation = [&ProjectSwitchContinuationCount] { ++ProjectSwitchContinuationCount; };
		const auto EnqueueContinuation = [&QueuedContinuations](std::function<void()> Action)
		{ QueuedContinuations.push_back(std::move(Action)); };
		const auto FailedProjectSwitchClose =
			ResolveDocumentCloseBatch(Manager, Registry, BatchIdentities, PendingProjectSwitchContinuation, EnqueueContinuation);
		PF_CHECK(Tests, !FailedProjectSwitchClose && PendingProjectSwitchContinuation && QueuedContinuations.empty());
		PF_CHECK(Tests, ProjectSwitchContinuationCount == 0 && ShutdownContinuationCount == 0 && CloseCount == 0 &&
							Manager.Find(IdentityA) == SessionA && Manager.Find(IdentityB) == SessionB);
		PendingProjectSwitchContinuation = {};
		PF_CHECK(Tests, Manager.Find(IdentityA) == SessionA && Manager.Find(IdentityB) == SessionB && ProjectSwitchContinuationCount == 0);
		std::function<void()> PendingShutdownContinuation = [&ShutdownContinuationCount] { ++ShutdownContinuationCount; };
		const auto FailedShutdownClose =
			ResolveDocumentCloseBatch(Manager, Registry, BatchIdentities, PendingShutdownContinuation, EnqueueContinuation);
		PF_CHECK(Tests, !FailedShutdownClose && PendingShutdownContinuation && QueuedContinuations.empty() &&
							ShutdownContinuationCount == 0 && Manager.Find(IdentityA) == SessionA && Manager.Find(IdentityB) == SessionB);
		StateB->FailClose = false;
		const auto RetriedCoordinatedClose =
			ResolveDocumentCloseBatch(Manager, Registry, BatchIdentities, PendingShutdownContinuation, EnqueueContinuation);
		PF_CHECK(Tests, RetriedCoordinatedClose.has_value());
		PF_CHECK(Tests, !PendingShutdownContinuation && QueuedContinuations.size() == 1 && ProjectSwitchContinuationCount == 0 &&
							ShutdownContinuationCount == 0);
		PF_CHECK(Tests, CloseCount == 2 && !Manager.Find(IdentityA) && !Manager.Find(IdentityB));
		QueuedContinuations.front()();
		PF_CHECK(Tests, ShutdownContinuationCount == 1 && ProjectSwitchContinuationCount == 0);
		PendingShutdownContinuation = [&ShutdownContinuationCount] { ShutdownContinuationCount += 100; };
		PendingShutdownContinuation = {};
		PF_CHECK(Tests, QueueDocumentCloseContinuation(true, PendingShutdownContinuation, EnqueueContinuation));
		PF_CHECK(Tests, QueuedContinuations.size() == 1 && ShutdownContinuationCount == 1);
	}
} // namespace

void RunEditorDocumentEditorRegistryTests(TestRunner& Tests)
{
	TestEditorDocumentEditorRegistry(Tests);
}
