#include "TestSupport.h"

#include "EditorConsoleHistory.h"
#include "Editor/EditorCommandHistory.h"
#include "Editor/EditorMaterialDraftStore.h"

int main()
{
	TestRunner Tests;
	RunEditorSceneEntityCommandTests(Tests);
	RunEditorSceneViewportInteractionTests(Tests);
	RunEditorViewportGizmoGeometryTests(Tests);
	RunEditorViewportGizmoInteractionTests(Tests);
	PulseForgeEditor::EditorConsoleHistory Console;
	std::vector<PulseForgeEditor::EditorConsoleMessage> Messages;
	uint64_t Revision = 0;
	Console.Append("APP", PulseForgeEditor::EditorConsoleSeverity::Info, "engine ready");
	PF_CHECK(Tests, Console.CopyMessagesIfChanged(Revision, Messages, Revision));
	PF_CHECK(Tests, Messages.size() == 1 && Messages.front().Text == "engine ready");

	PulseForgeEditor::EditorCommandHistory History;
	PF_CHECK(Tests, History.Begin("Rename", "before"));
	const auto Committed = History.Commit("after");
	PF_CHECK(Tests, Committed && *Committed && History.IsDirty() && History.CanUndo());
	std::string State = "after";
	const auto Undone = History.Undo(
		[&State](std::string_view Before) -> std::expected<void, std::string>
		{
			State.assign(Before);
			return {};
		});
	PF_CHECK(Tests, Undone && *Undone && State == "before" && !History.IsDirty());

	PulseForgeEditor::EditorMaterialDraftStore Drafts;
	PulseForge::MaterialAssetDesc Original;
	Original.RoughnessFactor = 0.6f;
	Drafts.PutDraft("project:material", Original);
	PF_CHECK(Tests, Drafts.BeginEdit("project:material", Original));
	Drafts.FindDraft("project:material")->RoughnessFactor = 0.25f;
	Drafts.FinalizePendingEdits(false);
	PF_CHECK(Tests, Drafts.IsDirty("project:material") && Drafts.CanUndo("project:material"));
	const auto DraftUndo = Drafts.ApplyHistoryStep("project:material", false);
	PF_CHECK(Tests, DraftUndo && *DraftUndo && !Drafts.IsDirty("project:material"));

	return Tests.Finish();
}
