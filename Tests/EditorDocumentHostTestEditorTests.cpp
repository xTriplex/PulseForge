#include "TestEditorSupport.h"

#include "EditorDocumentHostTestEditor.h"

namespace
{
	void TestDocumentHostTestEditor(TestRunner& Tests)
	{
		using namespace PulseForge;
		using namespace PulseForgeEditor;
		using FixtureEditor = PulseForgeEditor::Detail::EditorDocumentHostTestEditor;
		const auto Project = UUID::Generate();
		const auto OtherProject = UUID::Generate();
		const auto FixtureAssetIDs = FixtureEditor::FixtureAssetIDs();
		const auto AssetA = UUID::Parse(FixtureAssetIDs[0]);
		const auto AssetB = UUID::Parse(FixtureAssetIDs[1]);
		PF_CHECK(Tests, Project && OtherProject && AssetA && AssetB);
		if (!Project || !OtherProject || !AssetA || !AssetB) return;

		UUID ActiveProject = *Project;
		std::string ReportedError;
		EditorDocumentManager Documents;
		EditorDocumentEditorRegistry Editors;
		EditorStyle Style;
		FixtureEditor Fixture(
			Documents, Editors, Style, [&ReportedError](std::string Message) { ReportedError = std::move(Message); },
			[&ActiveProject] { return ActiveProject; });
		PF_CHECK(Tests, Fixture.Register().has_value());

		const EditorDocumentIdentity IdentityA{ *Project, *AssetA, EditorDocumentKind::Texture };
		const EditorDocumentIdentity IdentityB{ *Project, *AssetB, EditorDocumentKind::Texture };
		const EditorDocumentIdentity WrongProject{ *OtherProject, *AssetA, EditorDocumentKind::Texture };
		const EditorDocumentIdentity WrongKind{ *Project, *AssetA, EditorDocumentKind::Material };
		PF_CHECK(Tests, Fixture.IsFixtureIdentity(IdentityA) && Fixture.IsFixtureIdentity(IdentityB));
		PF_CHECK(Tests, !Fixture.IsFixtureIdentity(WrongProject) && !Fixture.IsFixtureIdentity(WrongKind));
		ActiveProject = *OtherProject;
		PF_CHECK(Tests, !Fixture.IsFixtureIdentity(IdentityA));
		ActiveProject = *Project;

		EditorDocumentEditorContext ContextA{ IdentityA, "FixtureProject", "Assets/FixtureA.texture", "Fixture A", nullptr };
		EditorDocumentEditorContext ContextB{ IdentityB, "FixtureProject", "Assets/FixtureB.texture", "Fixture B", nullptr };
		PF_CHECK(Tests, Editors.CanOpen(ContextA) && Editors.CanOpen(ContextB));
		const auto OpenA =
			Documents.Open(IdentityA, ContextA.ProjectRelativePath, ContextA.DisplayName, EditorDocumentOpenPreference::MainDocumentStrip);
		const auto OpenB =
			Documents.Open(IdentityB, ContextB.ProjectRelativePath, ContextB.DisplayName, EditorDocumentOpenPreference::MainDocumentStrip);
		PF_CHECK(Tests, OpenA && OpenB);
		auto* SessionA = Documents.Find(IdentityA);
		auto* SessionB = Documents.Find(IdentityB);
		PF_CHECK(Tests, SessionA && SessionB && Editors.Load(*SessionA, ContextA) && Editors.Load(*SessionB, ContextB));
		if (!SessionA || !SessionB) return;
		const EditorDocumentEditorState* StateA = SessionA->EditorState.get();
		const EditorDocumentEditorState* StateB = SessionB->EditorState.get();
		PF_CHECK(Tests, StateA && StateB && StateA != StateB);
		PF_CHECK(Tests, Fixture.InitializeSavedModel(*SessionA, 10) && Fixture.InitializeSavedModel(*SessionB, 30));
		PF_CHECK(Tests, !SessionA->IsDirty() && !SessionB->IsDirty());

		PF_CHECK(Tests, Fixture.EditValue(*SessionA, 11) && Fixture.EditValue(*SessionB, 31));
		PF_CHECK(Tests, SessionA->IsDirty() && SessionB->IsDirty());
		PF_CHECK(Tests, Fixture.FailNextSave(*SessionA) && !Editors.Save(*SessionA));
		const auto AfterSaveFailure = Fixture.InspectModel(*SessionA);
		PF_CHECK(Tests, SessionA->IsDirty() && SessionB->IsDirty() && AfterSaveFailure && AfterSaveFailure->Value == 11 &&
							AfterSaveFailure->UndoCount == 2 && ReportedError == "The document-host fixture save was rejected.");
		PF_CHECK(Tests, Editors.Save(*SessionA) && !SessionA->IsDirty() && SessionB->IsDirty());

		PF_CHECK(Tests, Editors.DispatchFocusedCommand(*SessionA, EditorDocumentCommand::Undo));
		const auto AfterUndo = Fixture.InspectModel(*SessionA);
		const auto AfterUndoB = Fixture.InspectModel(*SessionB);
		PF_CHECK(Tests, AfterUndo && AfterUndo->Value == 10 && AfterUndo->UndoCount == 1 && AfterUndo->RedoCount == 1);
		PF_CHECK(Tests, SessionA->IsDirty() && AfterUndoB && AfterUndoB->Value == 31 && SessionB->IsDirty());
		PF_CHECK(Tests, Editors.PrepareClose(*SessionA).has_value());
		PF_CHECK(Tests, Fixture.SetCloseFailure(*SessionA, true));
		PF_CHECK(Tests, !Editors.PrepareClose(*SessionA) && Documents.Find(IdentityA) == SessionA);
		PF_CHECK(Tests, Fixture.SetCloseFailure(*SessionA, false) && Editors.PrepareClose(*SessionA).has_value());
		PF_CHECK(Tests, Editors.Discard(*SessionA) && !SessionA->IsDirty());
		PF_CHECK(Tests, Fixture.InspectModel(*SessionA)->Value == 11 && Fixture.InspectModel(*SessionA)->UndoCount == 0);
		PF_CHECK(Tests, Editors.Discard(*SessionB) && !SessionB->IsDirty());
		PF_CHECK(Tests, Fixture.InspectModel(*SessionB)->Value == 30 && Fixture.InspectModel(*SessionB)->UndoCount == 0);

		const auto NativeHost = Documents.CreateNativeHost(EditorDocumentKind::Texture);
		PF_CHECK(Tests, NativeHost.has_value());
		if (NativeHost)
		{
			PF_CHECK(Tests, Documents.Transfer(IdentityA, *NativeHost, 0).has_value());
			PF_CHECK(Tests, Documents.Find(IdentityA) == SessionA && SessionA->EditorState.get() == StateA &&
								Fixture.InspectModel(*SessionA)->Value == 11);
			PF_CHECK(Tests, Documents.Transfer(IdentityA, EditorDocumentManager::MainHostID, 0).has_value());
			PF_CHECK(Tests, Documents.RemoveEmptyNativeHost(*NativeHost));
		}

		PF_CHECK(Tests, Editors.UpdateAssetLocation(*SessionA, "Assets/Renamed/FixtureA.texture", "Renamed Fixture A"));
		PF_CHECK(Tests, SessionA->ProjectRelativePath == "Assets/Renamed/FixtureA.texture" &&
							SessionA->DisplayName == "Renamed Fixture A" && SessionA->EditorState.get() == StateA);

		const auto ClearedFirstProject = Documents.ClearProject(*Project);
		(void)ClearedFirstProject;
		PF_CHECK(Tests, Documents.Find(IdentityA) == nullptr && Documents.Find(IdentityB) == nullptr);
		ActiveProject = *OtherProject;
		const EditorDocumentIdentity OtherProjectIdentity{ *OtherProject, *AssetA, EditorDocumentKind::Texture };
		EditorDocumentEditorContext OtherProjectContext{ OtherProjectIdentity, "OtherProject", "Assets/FixtureA.texture",
			"Other Project Fixture", nullptr };
		const auto OtherProjectOpen = Documents.Open(OtherProjectIdentity, OtherProjectContext.ProjectRelativePath,
			OtherProjectContext.DisplayName, EditorDocumentOpenPreference::MainDocumentStrip);
		auto* OtherProjectSession = Documents.Find(OtherProjectIdentity);
		PF_CHECK(Tests, OtherProjectOpen && OtherProjectSession && Editors.Load(*OtherProjectSession, OtherProjectContext));
		if (OtherProjectSession)
		{
			const auto OtherSnapshot = Fixture.InspectModel(*OtherProjectSession);
			PF_CHECK(Tests, OtherSnapshot && OtherSnapshot->Value == 0);
			PF_CHECK(Tests, Fixture.InitializeSavedModel(*OtherProjectSession, 99));
		}

		const auto ClearedOtherProject = Documents.ClearProject(*OtherProject);
		(void)ClearedOtherProject;
		PF_CHECK(Tests, Documents.Find(OtherProjectIdentity) == nullptr);
		ActiveProject = *Project;
		const auto RestoredOpen =
			Documents.Open(IdentityA, ContextA.ProjectRelativePath, ContextA.DisplayName, EditorDocumentOpenPreference::MainDocumentStrip);
		auto* RestoredSession = Documents.Find(IdentityA);
		PF_CHECK(Tests, RestoredOpen && RestoredSession && Editors.Load(*RestoredSession, ContextA));
		if (RestoredSession)
		{
			const auto RestoredSnapshot = Fixture.InspectModel(*RestoredSession);
			PF_CHECK(Tests, RestoredSnapshot && RestoredSnapshot->Value == 11);
		}
	}
} // namespace

void RunEditorDocumentHostTestEditorTests(TestRunner& Tests)
{
	TestDocumentHostTestEditor(Tests);
}
