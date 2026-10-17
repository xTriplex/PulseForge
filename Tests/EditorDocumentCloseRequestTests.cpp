#include "TestSupport.h"

#include "Editor/EditorDocumentCloseRequest.h"

#include <array>
#include <unordered_map>

void RunEditorDocumentCloseRequestTests(TestRunner& Tests)
{
	using namespace PulseForge;
	using namespace PulseForgeEditor;

	struct Model final : EditorDocumentEditorState
	{
		int Value = 0;
		bool FailClose = false;
		size_t* DestructionCount = nullptr;

		~Model() noexcept override
		{
			if (DestructionCount) ++*DestructionCount;
		}
	};

	const auto Project = UUID::Generate();
	const auto AssetA = UUID::Generate();
	const auto AssetB = UUID::Generate();
	PF_CHECK(Tests, Project && AssetA && AssetB);
	if (!Project || !AssetA || !AssetB) return;

	std::unordered_map<UUID, int, UUIDHash> SavedValues;
	size_t EditorStateDestructions = 0;
	EditorDocumentEditorRegistry Editors;
	EditorDocumentEditorAdapter Adapter;
	Adapter.Kind = EditorDocumentKind::Texture;
	Adapter.IsEligible = [](const EditorDocumentEditorContext& Context)
	{ return Context.Identity.Kind == EditorDocumentKind::Texture && Context.ProjectRelativePath.extension() == ".fixture"; };
	Adapter.LoadSession =
		[&SavedValues, &EditorStateDestructions](
			const EditorDocumentEditorContext& Context) -> std::expected<std::unique_ptr<EditorDocumentEditorState>, std::string>
	{
		auto State = std::make_unique<Model>();
		State->DestructionCount = &EditorStateDestructions;
		if (const auto Saved = SavedValues.find(Context.Identity.Asset); Saved != SavedValues.end()) State->Value = Saved->second;
		return std::unique_ptr<EditorDocumentEditorState>(std::move(State));
	};
	Adapter.DrawWorkspace = [](EditorDocumentSession&) {};
	Adapter.Save = [&SavedValues](EditorDocumentSession& Session, uint64_t)
	{
		auto* State = dynamic_cast<Model*>(Session.EditorState.get());
		if (!State) return false;
		SavedValues[Session.Identity.Asset] = State->Value;
		return true;
	};
	Adapter.Discard = [&SavedValues](EditorDocumentSession& Session)
	{
		auto* State = dynamic_cast<Model*>(Session.EditorState.get());
		if (!State) return false;
		State->Value = SavedValues[Session.Identity.Asset];
		return true;
	};
	Adapter.FocusedCommand = [](EditorDocumentSession&, EditorDocumentCommand) { return false; };
	Adapter.AssetLocationChanged = [](EditorDocumentSession&, const std::filesystem::path&, std::string_view) { return true; };
	Adapter.PrepareClose = [](const EditorDocumentCloseContext& Context) -> std::expected<void, std::string>
	{
		const auto* State = dynamic_cast<const Model*>(Context.EditorState);
		if (State && State->FailClose) return std::unexpected("close rejected by fixture");
		return {};
	};
	PF_CHECK(Tests, Editors.Register(std::move(Adapter)).has_value());

	const EditorDocumentIdentity IdentityA{ *Project, *AssetA, EditorDocumentKind::Texture };
	const EditorDocumentIdentity IdentityB{ *Project, *AssetB, EditorDocumentKind::Texture };
	EditorDocumentManager Documents;
	const auto OpenA = Documents.Open(IdentityA, "Assets/A.fixture", "A", EditorDocumentOpenPreference::MainDocumentStrip);
	const auto OpenB = Documents.Open(IdentityB, "Assets/B.fixture", "B", EditorDocumentOpenPreference::MainDocumentStrip);
	PF_CHECK(Tests, OpenA && OpenB);
	if (!OpenA || !OpenB) return;
	EditorDocumentEditorContext ContextA{ IdentityA, {}, "Assets/A.fixture", "A", nullptr };
	EditorDocumentEditorContext ContextB{ IdentityB, {}, "Assets/B.fixture", "B", nullptr };
	auto* SessionA = Documents.Find(IdentityA);
	auto* SessionB = Documents.Find(IdentityB);
	PF_CHECK(Tests, SessionA && SessionB && Editors.Load(*SessionA, ContextA) && Editors.Load(*SessionB, ContextB));
	if (!SessionA || !SessionB || !SessionA->EditorState || !SessionB->EditorState) return;
	auto* StateA = dynamic_cast<Model*>(SessionA->EditorState.get());
	auto* StateB = dynamic_cast<Model*>(SessionB->EditorState.get());
	PF_CHECK(Tests, StateA && StateB);
	if (!StateA || !StateB) return;
	StateA->Value = 17;
	PF_CHECK(Tests, Documents.MarkEdited(IdentityA));
	StateB->FailClose = true;

	EditorDocumentCloseRequest Request;
	size_t ContinuationCount = 0;
	Request.BeginCloseAllDocuments(
		{ IdentityA, IdentityB }, std::string(EditorDocumentManager::MainHostID), [&ContinuationCount] { ++ContinuationCount; });
	PF_CHECK(Tests, Request.HasPendingRequest() && Request.ClosesDocuments());
	PF_CHECK(Tests, Request.ConsumeDialogOpenRequest() && !Request.ConsumeDialogOpenRequest());
	const std::array<EditorDocumentIdentity, 1> DirtyBeforeSave = { IdentityA };
	std::vector<EditorDocumentCloseRequest::Action> Queued;
	const auto Enqueue = [&Queued](EditorDocumentCloseRequest::Action Action) { Queued.push_back(std::move(Action)); };
	const auto ResolveChanges = [&Documents, &Editors](std::span<const EditorDocumentIdentity> Identities,
									EditorDocumentCloseChoice Choice) -> std::expected<void, std::string>
	{
		for (const EditorDocumentIdentity& Identity : Identities)
		{
			auto* Session = Documents.Find(Identity);
			if (!Session || !Session->IsDirty()) continue;
			if (Choice == EditorDocumentCloseChoice::Save)
			{
				if (!Editors.Save(*Session) || Session->IsDirty()) return std::unexpected("save rejected by fixture");
			}
			else if (!Editors.Discard(*Session)) return std::unexpected("discard rejected by fixture");
		}
		return {};
	};
	const auto FailedSavedClose =
		Request.Resolve(Documents, Editors, EditorDocumentCloseChoice::Save, DirtyBeforeSave, ResolveChanges, Enqueue);
	PF_CHECK(Tests, !FailedSavedClose && Request.HasPendingRequest() && !Request.GetError().empty());
	PF_CHECK(Tests, Documents.Find(IdentityA) == SessionA && Documents.Find(IdentityB) == SessionB);
	PF_CHECK(Tests, !SessionA->IsDirty() && StateA->Value == 17 && SavedValues[*AssetA] == 17);
	PF_CHECK(Tests, EditorStateDestructions == 0 && Queued.empty() && ContinuationCount == 0);
	PF_CHECK(Tests, Request.ShouldOfferCloseRetry(true));
	const std::array<EditorDocumentIdentity, 0> NoDirtyDocuments{};
	StateB->FailClose = false;
	const auto RetriedClose =
		Request.Resolve(Documents, Editors, EditorDocumentCloseChoice::Save, NoDirtyDocuments, ResolveChanges, Enqueue);
	PF_CHECK(Tests, RetriedClose && RetriedClose->DocumentsClosed && !Request.HasPendingRequest());
	PF_CHECK(Tests, !Documents.Find(IdentityA) && !Documents.Find(IdentityB) && EditorStateDestructions == 2);
	PF_CHECK(Tests, Queued.size() == 1 && ContinuationCount == 0);
	if (!Queued.empty()) Queued.front()();
	PF_CHECK(Tests, ContinuationCount == 1);

	const auto ReopenedA = Documents.Open(IdentityA, "Assets/A.fixture", "A", EditorDocumentOpenPreference::MainDocumentStrip);
	PF_CHECK(Tests, ReopenedA.has_value());
	if (!ReopenedA) return;
	SessionA = Documents.Find(IdentityA);
	PF_CHECK(Tests, SessionA && Editors.Load(*SessionA, ContextA));
	if (!SessionA) return;
	StateA = dynamic_cast<Model*>(SessionA->EditorState.get());
	PF_CHECK(Tests, StateA && StateA->Value == 17);
	if (!StateA) return;
	StateA->Value = 23;
	PF_CHECK(Tests, Documents.MarkEdited(IdentityA));
	StateA->FailClose = true;
	const EditorDocumentEditorState* const OriginalState = SessionA->EditorState.get();
	Request.BeginCloseDocuments(
		{ IdentityA }, std::string(EditorDocumentManager::MainHostID), [&ContinuationCount] { ContinuationCount += 100; });
	const std::array<EditorDocumentIdentity, 1> DirtyBeforeDiscard = { IdentityA };
	size_t DiscardAttempts = 0;
	const auto FailingClose = Request.Resolve(
		Documents, Editors, EditorDocumentCloseChoice::Discard, DirtyBeforeDiscard,
		[&ResolveChanges, &DiscardAttempts](std::span<const EditorDocumentIdentity> Identities, EditorDocumentCloseChoice Choice)
		{
			++DiscardAttempts;
			return ResolveChanges(Identities, Choice);
		},
		Enqueue);
	PF_CHECK(Tests, !FailingClose && DiscardAttempts == 0 && Request.HasPendingRequest());
	PF_CHECK(Tests, Documents.Find(IdentityA) == SessionA && SessionA->EditorState.get() == OriginalState);
	PF_CHECK(Tests, SessionA->IsDirty() && StateA->Value == 23 && ContinuationCount == 1 && Queued.size() == 1);
	Request.Cancel();
	PF_CHECK(Tests, !Request.HasPendingRequest() && Request.GetError().empty());
	PF_CHECK(Tests, Documents.Find(IdentityA) == SessionA && SessionA->IsDirty() && StateA->Value == 23);
}
