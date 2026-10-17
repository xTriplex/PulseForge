#include "TestEditorSupport.h"
#include "EditorConsoleHistory.h"
#include "EditorConsoleSeverity.h"
#include "Editor/EditorMaterialDraftStore.h"
#include "Editor/EditorStatusPanel.h"

namespace
{
	void TestEditorConsoleHistory(TestRunner& Tests)
	{
		PulseForgeEditor::EditorConsoleHistory History;
		std::vector<PulseForgeEditor::EditorConsoleMessage> Messages;
		uint64_t Revision = 0;
		History.Append("APP", PulseForgeEditor::EditorConsoleSeverity::Info, "first\r\nmessage");
		PF_CHECK(Tests, History.CopyMessagesIfChanged(Revision, Messages, Revision));
		PF_CHECK(Tests, Messages.size() == 1);
		PF_CHECK(Tests, Messages[0].Sequence == 1);
		PF_CHECK(Tests, Messages[0].Logger == "APP");
		PF_CHECK(Tests, Messages[0].Severity == PulseForgeEditor::EditorConsoleSeverity::Info);
		PF_CHECK(Tests, Messages[0].Text == "first  message");
		PF_CHECK(Tests, !History.CopyMessagesIfChanged(Revision, Messages, Revision));

		const std::string Oversized(PulseForgeEditor::EditorConsoleHistory::MaxMessageBytes + 32, 'x');
		History.Append("PULSEFORGE", PulseForgeEditor::EditorConsoleSeverity::Error, Oversized);
		PF_CHECK(Tests, History.CopyMessagesIfChanged(Revision, Messages, Revision));
		PF_CHECK(Tests, Messages.back().Text.size() == PulseForgeEditor::EditorConsoleHistory::MaxMessageBytes + 12);
		PF_CHECK(Tests, Messages.back().Text.ends_with(" [truncated]"));

		for (size_t Index = 0; Index < PulseForgeEditor::EditorConsoleHistory::Capacity + 1; ++Index)
			History.Append("TEST", PulseForgeEditor::EditorConsoleSeverity::Debug, "entry");
		PF_CHECK(Tests, History.CopyMessagesIfChanged(Revision, Messages, Revision));
		PF_CHECK(Tests, Messages.size() == PulseForgeEditor::EditorConsoleHistory::Capacity);
		PF_CHECK(Tests, Messages.front().Sequence == 4);
		PF_CHECK(Tests, Messages.back().Sequence == PulseForgeEditor::EditorConsoleHistory::Capacity + 3);

		History.Clear();
		PF_CHECK(Tests, History.CopyMessagesIfChanged(Revision, Messages, Revision));
		PF_CHECK(Tests, Messages.empty());
	}

	void TestEditorConsoleSeverityConversion(TestRunner& Tests)
	{
		using PulseForge::LogLevel;
		using PulseForgeEditor::EditorConsoleSeverity;
		using PulseForgeEditor::Detail::ConvertConsoleSeverity;

		PF_CHECK(Tests, ConvertConsoleSeverity(LogLevel::Trace) == EditorConsoleSeverity::Trace);
		PF_CHECK(Tests, ConvertConsoleSeverity(LogLevel::Debug) == EditorConsoleSeverity::Debug);
		PF_CHECK(Tests, ConvertConsoleSeverity(LogLevel::Info) == EditorConsoleSeverity::Info);
		PF_CHECK(Tests, ConvertConsoleSeverity(LogLevel::Warning) == EditorConsoleSeverity::Warning);
		PF_CHECK(Tests, ConvertConsoleSeverity(LogLevel::Error) == EditorConsoleSeverity::Error);
		PF_CHECK(Tests, ConvertConsoleSeverity(LogLevel::Critical) == EditorConsoleSeverity::Critical);
		PF_CHECK(Tests, ConvertConsoleSeverity(LogLevel::Unknown) == EditorConsoleSeverity::Unknown);
		PF_CHECK(Tests, ConvertConsoleSeverity(static_cast<LogLevel>(127)) == EditorConsoleSeverity::Unknown);
	}

	void TestEditorContentBrowserState(TestRunner& Tests)
	{
		using namespace PulseForge;
		using namespace PulseForgeEditor;
		EditorContentBrowserState Browser;
		Browser.SetAssets({ { AssetID{ 0, 1 }, "Assets/Textures/Sky/blue.png" }, { AssetID{ 0, 2 }, "Assets/Textures/red.png" },
			{ AssetID{ 0, 3 }, "Assets/Materials/metal.material" }, { AssetID{ 0, 4 }, "Outside/loose.scene" } });

		const EditorContentFolderNode& Root = Browser.FolderRoot();
		PF_CHECK(Tests, Root.Name == "Assets");
		PF_CHECK(Tests, Root.Children.size() == 2);
		PF_CHECK(Tests, Root.Children[0].Name == "Materials");
		PF_CHECK(Tests, Root.Children[1].Name == "Textures");
		PF_CHECK(Tests, Browser.FindFolder("Assets/Textures/Sky") != nullptr);
		PF_CHECK(Tests, !Browser.IsKnownFolder("Assets/Missing"));
		PF_CHECK(Tests, Browser.Assets().size() == 4);
		PF_CHECK(Tests, Browser.AssetPresentations().size() == Browser.Assets().size());
		PF_CHECK(Tests, Browser.AssetPresentations()[0].Identifier == Browser.Assets()[0].ID.ToString());
		PF_CHECK(Tests, Browser.AssetPresentations()[0].Name == "blue.png");
		PF_CHECK(Tests, Browser.AssetPresentations()[0].FullPath == "Assets/Textures/Sky/blue.png");
		PF_CHECK(Tests, Browser.AssetPresentations()[0].Extension == ".png");
		PF_CHECK(Tests, Browser.AssetPresentations()[0].Icon == EditorIcon::Image);
		PF_CHECK(Tests, Browser.AssetPresentations()[2].Icon == EditorIcon::Material);

		Browser.RebuildFilteredAssetIndices();
		PF_CHECK(Tests, Browser.FilteredAssetIndices().empty());

		Browser.SetCurrentFolder("Assets/Textures");
		Browser.RebuildFilteredAssetIndices();
		PF_CHECK(Tests, Browser.FilteredAssetIndices().size() == 1);
		PF_CHECK(Tests, Browser.Assets()[Browser.FilteredAssetIndices()[0]].ProjectRelativePath == "Assets/Textures/red.png");

		Browser.SearchText() = "BLUE";
		Browser.RebuildFilteredAssetIndices();
		PF_CHECK(Tests, Browser.FilteredAssetIndices().size() == 1);
		PF_CHECK(Tests, Browser.Assets()[Browser.FilteredAssetIndices()[0]].ID == AssetID(0, 1));

		Browser.SelectedFolder() = std::filesystem::path("Assets/Textures/Sky");
		Browser.RebaseFoldersAfterMove("Assets/Textures", "Assets/Art/Textures");
		PF_CHECK(Tests, Browser.CurrentFolder() == "Assets/Art/Textures");
		PF_CHECK(Tests, Browser.SelectedFolder() && *Browser.SelectedFolder() == "Assets/Art/Textures/Sky");

		Browser.SetAssets({ { AssetID{ 0, 3 }, "Assets/Materials/metal.material" } });
		PF_CHECK(Tests, Browser.AssetPresentations().size() == 1);
		PF_CHECK(Tests, Browser.AssetPresentations()[0].Name == "metal.material");
		PF_CHECK(Tests, Browser.CurrentFolder() == "Assets");
		PF_CHECK(Tests, !Browser.SelectedFolder());
		Browser.GridView() = false;
		Browser.ResetNavigation();
		PF_CHECK(Tests, Browser.CurrentFolder() == "Assets");
		PF_CHECK(Tests, Browser.SearchText().empty());
		PF_CHECK(Tests, Browser.IsGridView());

		Browser.SelectedAsset() = AssetID{ 0, 3 };
		Browser.ClearProject();
		PF_CHECK(Tests, Browser.Assets().empty());
		PF_CHECK(Tests, Browser.AssetPresentations().empty());
		PF_CHECK(Tests, !Browser.SelectedAsset());
		PF_CHECK(Tests, Browser.FolderRoot().Children.empty());
	}

	void TestEditorStatusPanel(TestRunner& Tests)
	{
		using namespace PulseForgeEditor;
		EditorStatusPanel Status;
		PF_CHECK(Tests, Status.Message() == "Create or open a project to begin.");
		PF_CHECK(Tests, Status.Severity() == EditorStatusSeverity::Information);
		Status.SetStatus("Scene saved.");
		PF_CHECK(Tests, Status.Message() == "Scene saved.");
		PF_CHECK(Tests, Status.Severity() == EditorStatusSeverity::Information);
		Status.SetWarning("The startup scene is unavailable.");
		PF_CHECK(Tests, Status.Message() == "The startup scene is unavailable.");
		PF_CHECK(Tests, Status.Severity() == EditorStatusSeverity::Warning);
		Status.SetError("Scene save failed.");
		PF_CHECK(Tests, Status.Message() == "Scene save failed.");
		PF_CHECK(Tests, Status.Severity() == EditorStatusSeverity::Error);
	}

	void TestEditorAssetWorkflowState(TestRunner& Tests)
	{
		using namespace PulseForge;
		using namespace PulseForgeEditor;
		EditorAssetWorkflowState Workflow;
		const AssetRecord Material{ AssetID{ 2, 1 }, "Assets/Materials/Rough Clay.material" };

		Workflow.BeginAssetDestination(Material, false);
		PF_CHECK(Tests, Workflow.TakeDestinationDialogRequest());
		PF_CHECK(Tests, !Workflow.TakeDestinationDialogRequest());
		PF_CHECK(Tests, Workflow.DestinationAsset() == Material.ID);
		PF_CHECK(Tests, !Workflow.IsDuplicate());
		PF_CHECK(Tests, Workflow.CreateKind() == ProjectAssetDestinationCreateKind::None);
		PF_CHECK(Tests, Workflow.DestinationFolder() == "Assets/Materials");
		PF_CHECK(Tests, std::string(Workflow.FilenameBuffer().data()) == "Rough Clay.material");
		Workflow.ValidationError() = "invalid destination";

		Workflow.BeginAssetDestination(Material, true);
		PF_CHECK(Tests, Workflow.TakeDestinationDialogRequest());
		PF_CHECK(Tests, Workflow.IsDuplicate());
		PF_CHECK(Tests, std::string(Workflow.FilenameBuffer().data()) == "Rough Clay Copy.material");
		PF_CHECK(Tests, Workflow.ValidationError().empty());

		Workflow.BeginSceneDestination(false, std::nullopt);
		PF_CHECK(Tests, Workflow.CreateKind() == ProjectAssetDestinationCreateKind::NewScene);
		PF_CHECK(Tests, Workflow.DestinationFolder() == "Assets");
		PF_CHECK(Tests, std::string(Workflow.FilenameBuffer().data()) == "NewScene.scene");

		const AssetRecord Scene{ AssetID{ 2, 2 }, "Assets/Scenes/Overview.scene" };
		Workflow.BeginSceneDestination(true, Scene);
		PF_CHECK(Tests, Workflow.CreateKind() == ProjectAssetDestinationCreateKind::SaveSceneAs);
		PF_CHECK(Tests, Workflow.DestinationFolder() == "Assets/Scenes");
		PF_CHECK(Tests, std::string(Workflow.FilenameBuffer().data()) == "Overview Copy.scene");

		Workflow.BeginPrefabDestination(UUID{ 9, 4 });
		PF_CHECK(Tests, Workflow.CreateKind() == ProjectAssetDestinationCreateKind::Prefab);
		PF_CHECK(Tests, (Workflow.PrefabRoot() == UUID{ 9, 4 }));
		PF_CHECK(Tests, std::string(Workflow.FilenameBuffer().data()) == "NewPrefab.prefab");
		Workflow.DismissDestination();
		PF_CHECK(Tests, !Workflow.DestinationAsset());
		PF_CHECK(Tests, !Workflow.PrefabRoot());
		PF_CHECK(Tests, Workflow.CreateKind() == ProjectAssetDestinationCreateKind::None);
		PF_CHECK(Tests, Workflow.ValidationError().empty());

		Workflow.BeginDelete(Material.ID);
		PF_CHECK(Tests, Workflow.TakeDeleteDialogRequest());
		PF_CHECK(Tests, Workflow.PendingDeleteAsset() == Material.ID);
		PF_CHECK(Tests, !Workflow.TakeDeleteDialogRequest());
		Workflow.DismissDelete();
		PF_CHECK(Tests, !Workflow.PendingDeleteAsset());
	}

	void TestEditorMaterialDraftStore(TestRunner& Tests)
	{
		PulseForgeEditor::EditorMaterialDraftStore Drafts;
		const std::string FirstKey = "project-a:material-1";
		const std::string SecondKey = "project-a:material-2";
		const std::string OtherProjectKey = "project-b:material-1";
		PulseForge::MaterialAssetDesc FirstMaterial;
		FirstMaterial.BaseColorFactor = { 0.2f, 0.4f, 0.6f, 1.0f };
		PulseForge::MaterialAssetDesc SecondMaterial;
		SecondMaterial.RoughnessFactor = 0.35f;
		Drafts.PutDraft(FirstKey, FirstMaterial);
		Drafts.PutDraft(SecondKey, SecondMaterial);
		PF_CHECK(Tests, Drafts.ContainsDraft(FirstKey));
		PF_CHECK(Tests, !Drafts.IsDirty(FirstKey));

		PF_CHECK(Tests, Drafts.BeginEdit(FirstKey, FirstMaterial));
		Drafts.FindDraft(FirstKey)->BaseColorFactor.x = 0.8f;
		Drafts.FinalizePendingEdits(true);
		PF_CHECK(Tests, Drafts.IsDirty(FirstKey));
		PF_CHECK(Tests, !Drafts.CanUndo(FirstKey));
		Drafts.FinalizePendingEdits(false);
		PF_CHECK(Tests, Drafts.CanUndo(FirstKey));
		PF_CHECK(Tests, Drafts.IsDirty(FirstKey));

		const auto UndoChanged = Drafts.ApplyHistoryStep(FirstKey, false);
		PF_CHECK(Tests, UndoChanged.has_value() && *UndoChanged);
		PF_CHECK(Tests, !Drafts.IsDirty(FirstKey));
		PF_CHECK(Tests, Drafts.FindDraft(FirstKey)->BaseColorFactor.x == 0.2f);
		const auto RedoChanged = Drafts.ApplyHistoryStep(FirstKey, true);
		PF_CHECK(Tests, RedoChanged.has_value() && *RedoChanged);
		PF_CHECK(Tests, Drafts.IsDirty(FirstKey));
		PF_CHECK(Tests, Drafts.FindDraft(FirstKey)->BaseColorFactor.x == 0.8f);
		Drafts.MarkSaved(FirstKey);
		PF_CHECK(Tests, !Drafts.IsDirty(FirstKey));

		PulseForge::MaterialAssetDesc Replacement;
		Replacement.BaseColorFactor = { 0.7f, 0.1f, 0.3f, 1.0f };
		Drafts.ReplaceDraft(FirstKey, Replacement);
		PF_CHECK(Tests, !Drafts.IsDirty(FirstKey));
		PF_CHECK(Tests, !Drafts.CanUndo(FirstKey));
		PF_CHECK(Tests, Drafts.FindDraft(FirstKey)->BaseColorFactor.x == 0.7f);

		Drafts.SetError(FirstKey, "load failed");
		PF_CHECK(Tests, Drafts.FindError(FirstKey) && *Drafts.FindError(FirstKey) == "load failed");
		Drafts.ClearError(FirstKey);
		PF_CHECK(Tests, Drafts.FindError(FirstKey) == nullptr);

		Drafts.PutDraft(OtherProjectKey, FirstMaterial);
		PF_CHECK(Tests, Drafts.BeginEdit(OtherProjectKey, FirstMaterial));
		Drafts.FindDraft(OtherProjectKey)->MetallicFactor = 0.9f;
		Drafts.FinalizePendingEdits(false);
		Drafts.ClearProject("project-a:");
		PF_CHECK(Tests, !Drafts.ContainsDraft(FirstKey));
		PF_CHECK(Tests, !Drafts.ContainsDraft(SecondKey));
		PF_CHECK(Tests, Drafts.ContainsDraft(OtherProjectKey));
		PF_CHECK(Tests, Drafts.IsDirty(OtherProjectKey));
	}
} // namespace

void RunEditorPanelStateTests(TestRunner& Tests)
{
	TestEditorConsoleHistory(Tests);
	TestEditorConsoleSeverityConversion(Tests);
	TestEditorContentBrowserState(Tests);
	TestEditorStatusPanel(Tests);
	TestEditorAssetWorkflowState(Tests);
	TestEditorMaterialDraftStore(Tests);
}
