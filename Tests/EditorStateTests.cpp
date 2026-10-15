#include "TestEditorSupport.h"
#include "EditorConsoleHistory.h"
#include "Editor/EditorHierarchyPanelPolicy.h"
#include "Editor/EditorEntityClipboard.h"
#include "Editor/EditorStatusPanel.h"
#include "EditorMaterialDraftStore.h"

#include <nlohmann/json.hpp>

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

	void TestEditorContentBrowserState(TestRunner& Tests)
	{
		using namespace PulseForge;
		using namespace PulseForgeEditor;
		EditorContentBrowserState Browser;
		Browser.SetAssets({
			{ AssetID{ 0, 1 }, "Assets/Textures/Sky/blue.png" },
			{ AssetID{ 0, 2 }, "Assets/Textures/red.png" },
			{ AssetID{ 0, 3 }, "Assets/Materials/metal.material" },
			{ AssetID{ 0, 4 }, "Outside/loose.scene" }
		});

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
		PF_CHECK(Tests, Browser.Assets()[Browser.FilteredAssetIndices()[0]].ProjectRelativePath ==
			"Assets/Textures/red.png");

		Browser.SearchText() = "BLUE";
		Browser.RebuildFilteredAssetIndices();
		PF_CHECK(Tests, Browser.FilteredAssetIndices().size() == 1);
		PF_CHECK(Tests, Browser.Assets()[Browser.FilteredAssetIndices()[0]].ID == AssetID(0, 1));

		Browser.SelectedFolder() = std::filesystem::path("Assets/Textures/Sky");
		Browser.RebaseFoldersAfterMove("Assets/Textures", "Assets/Art/Textures");
		PF_CHECK(Tests, Browser.CurrentFolder() == "Assets/Art/Textures");
		PF_CHECK(Tests, Browser.SelectedFolder() &&
			*Browser.SelectedFolder() == "Assets/Art/Textures/Sky");

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

	void TestEditorEntityClipboard(TestRunner& Tests)
	{
		using namespace PulseForge;
		using namespace PulseForgeEditor;
		using Json = nlohmann::ordered_json;
		const UUID Project{ 11, 1 };
		const AssetID SourceScene{ 22, 2 };
		const UUID Root{ 33, 3 };
		const UUID Child{ 44, 4 };
		const UUID Other{ 55, 5 };
		const UUID ExternalParent{ 66, 6 };
		const std::string Source = Json{
			{ "entities", Json::array({
				Json{ { "uuid", Root.ToString() }, { "tag", Json{ { "name", "Root" } } },
					{ "camera", Json{ { "primary", true } } } },
				Json{ { "uuid", Child.ToString() }, { "tag", Json{ { "name", "Child" } } },
					{ "parent", Root.ToString() }, { "audioListener", Json{ { "primary", true } } } },
				Json{ { "uuid", Other.ToString() }, { "tag", Json{ { "name", "Other" } } } }
			}) }
		}.dump();
		const std::array Roots{ Root };
		const std::array Included{ Root, Child };
		auto ClipboardResult = EditorEntityClipboard::Capture(Project, SourceScene, Roots, Included, Source);
		PF_CHECK(Tests, ClipboardResult.has_value());
		if (!ClipboardResult)
			return;
		EditorEntityClipboard Clipboard = std::move(*ClipboardResult);
		PF_CHECK(Tests, Clipboard.HasData());
		PF_CHECK(Tests, Clipboard.CanPaste(Project));
		PF_CHECK(Tests, !Clipboard.CanPaste(UUID{ 12, 1 }));

		const UUID Existing{ 77, 7 };
		const UUID NewRoot{ 88, 8 };
		const UUID NewChild{ 99, 9 };
		const std::string Destination = Json{
			{ "entities", Json::array({
				Json{ { "uuid", Existing.ToString() }, { "tag", Json{ { "name", "Root Copy" } } } }
			}) }
		}.dump();
		const std::array Generated{ Existing, NewRoot, NewChild };
		size_t GeneratedIndex = 0;
		auto Prepared = Clipboard.PreparePaste(Project, SourceScene, Destination, {}, [&]()
			-> std::expected<UUID, std::string>
		{
			return Generated[GeneratedIndex++];
		});
		PF_CHECK(Tests, Prepared.has_value());
		if (Prepared)
		{
			PF_CHECK(Tests, Prepared->EntityCount == 2);
			PF_CHECK(Tests, Prepared->RootIdentifiers == std::vector<UUID>{ NewRoot });
			const Json Pasted = Json::parse(Prepared->SerializedScene);
			PF_CHECK(Tests, Pasted["entities"].size() == 3);
			PF_CHECK(Tests, Pasted["entities"][1]["uuid"] == NewRoot.ToString());
			PF_CHECK(Tests, Pasted["entities"][1]["tag"]["name"] == "Root Copy 2");
			PF_CHECK(Tests, Pasted["entities"][1]["camera"]["primary"] == false);
			PF_CHECK(Tests, Pasted["entities"][2]["uuid"] == NewChild.ToString());
			PF_CHECK(Tests, Pasted["entities"][2]["parent"] == NewRoot.ToString());
			PF_CHECK(Tests, Pasted["entities"][2]["audioListener"]["primary"] == false);
		}
		size_t CrossSceneIdentifierIndex = 0;
		PF_CHECK(Tests, Clipboard.PreparePaste(Project, AssetID{ 23, 2 }, Destination, {}, [&]()
			-> std::expected<UUID, std::string>
		{
			return Generated[CrossSceneIdentifierIndex++ + 1];
		}).has_value());
		PF_CHECK(Tests, !Clipboard.PreparePaste(UUID{ 12, 1 }, SourceScene, Destination, {}).has_value());
		PF_CHECK(Tests, !Clipboard.PreparePaste(Project, SourceScene, Destination, {}, []()
			-> std::expected<UUID, std::string>
		{
			return std::unexpected("identifier generation failed");
		}).has_value());

		const std::string ExternalSource = Json{
			{ "entities", Json::array({
				Json{ { "uuid", ExternalParent.ToString() }, { "tag", Json{ { "name", "Parent" } } } },
				Json{ { "uuid", Other.ToString() }, { "tag", Json{ { "name", "Child" } } },
					{ "parent", ExternalParent.ToString() } }
			}) }
		}.dump();
		const std::array ExternalRoots{ Other };
		const std::array ExternalIncluded{ Other };
		auto ExternalClipboard = EditorEntityClipboard::Capture(
			Project, SourceScene, ExternalRoots, ExternalIncluded, ExternalSource);
		PF_CHECK(Tests, ExternalClipboard.has_value());
		if (ExternalClipboard)
		{
			const std::string TargetWithoutParent = Json{ { "entities", Json::array() } }.dump();
			PF_CHECK(Tests, !ExternalClipboard->PreparePaste(
				Project, AssetID{ 23, 2 }, TargetWithoutParent, {}).has_value());
			const std::string TargetWithParent = Json{
				{ "entities", Json::array({ Json{ { "uuid", ExternalParent.ToString() } } }) }
			}.dump();
			PF_CHECK(Tests, ExternalClipboard->PreparePaste(
				Project, SourceScene, TargetWithParent, {}).has_value());
		}

		const std::string AssetSource = Json{
			{ "entities", Json::array({ Json{
				{ "uuid", Other.ToString() },
				{ "meshRenderer", Json{ { "meshAsset", UUID{ 101, 10 }.ToString() } } }
			} }) }
		}.dump();
		const std::array AssetRoots{ Other };
		auto AssetClipboard = EditorEntityClipboard::Capture(Project, SourceScene, AssetRoots, AssetRoots, AssetSource);
		PF_CHECK(Tests, AssetClipboard.has_value());
		if (AssetClipboard)
		{
			const std::string EmptyScene = Json{ { "entities", Json::array() } }.dump();
			PF_CHECK(Tests, !AssetClipboard->PreparePaste(
				Project, SourceScene, EmptyScene, [](const UUID&) { return false; }).has_value());
			PF_CHECK(Tests, AssetClipboard->PreparePaste(
				Project, SourceScene, EmptyScene, [](const UUID&) { return true; }).has_value());
		}
		PF_CHECK(Tests, !EditorEntityClipboard::Capture(
			Project, SourceScene, Roots, Included, "not-json").has_value());
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

	void TestEditorSceneHierarchyOperations(TestRunner& Tests)
	{
		using namespace PulseForge;
		using namespace PulseForgeEditor;
		Scene SceneValue;
		auto Root = SceneValue.CreateEntity("Root");
		auto Child = SceneValue.CreateEntity("Child");
		auto Grandchild = SceneValue.CreateEntity("Grandchild");
		auto Separate = SceneValue.CreateEntity("Separate");
		PF_CHECK(Tests, Root && Child && Grandchild && Separate);
		if (!Root || !Child || !Grandchild || !Separate)
			return;
		PF_CHECK(Tests, Child->SetParent(*Root));
		PF_CHECK(Tests, Grandchild->SetParent(*Child));

		const std::array Selected{ Root->GetUUID(), Child->GetUUID(), Separate->GetUUID() };
		const auto Roots = EditorSceneHierarchyOperations::GetTopLevelSelection(SceneValue, Selected);
		PF_CHECK(Tests, Roots && Roots->size() == 2);
		if (!Roots)
			return;
		PF_CHECK(Tests, (*Roots)[0].GetUUID() == Root->GetUUID());
		PF_CHECK(Tests, (*Roots)[1].GetUUID() == Separate->GetUUID());
		const auto Subtree = EditorSceneHierarchyOperations::CollectSubtrees(
			std::span<const Entity>(&(*Roots)[0], 1));
		PF_CHECK(Tests, Subtree && Subtree->size() == 3);
		if (Subtree)
		{
			PF_CHECK(Tests, (*Subtree)[0].GetUUID() == Root->GetUUID());
			PF_CHECK(Tests, (*Subtree)[1].GetUUID() == Child->GetUUID());
			PF_CHECK(Tests, (*Subtree)[2].GetUUID() == Grandchild->GetUUID());
		}
		PF_CHECK(Tests, EditorSceneHierarchyOperations::MakeUniqueName(SceneValue, "Root") == "Root 2");

		const std::array DuplicationRoots{ *Root };
		const auto Duplicated = EditorSceneHierarchyOperations::DuplicateSubtrees(SceneValue, DuplicationRoots);
		PF_CHECK(Tests, Duplicated && Duplicated->size() == 1);
		PF_CHECK(Tests, SceneValue.GetEntityCount() == 7);
		if (!Duplicated || Duplicated->empty())
			return;
		const auto DuplicatedRoot = SceneValue.FindEntity(Duplicated->front());
		PF_CHECK(Tests, DuplicatedRoot.has_value());
		if (!DuplicatedRoot)
			return;
		PF_CHECK(Tests, DuplicatedRoot->GetTag()->Name == "Root Copy");
		const auto DuplicatedChildren = DuplicatedRoot->GetChildren();
		PF_CHECK(Tests, DuplicatedChildren && DuplicatedChildren->size() == 1);
		if (!DuplicatedChildren || DuplicatedChildren->empty())
			return;
		PF_CHECK(Tests, DuplicatedChildren->front().GetTag()->Name == "Child Copy");
		const auto DuplicatedGrandchildren = DuplicatedChildren->front().GetChildren();
		PF_CHECK(Tests, DuplicatedGrandchildren && DuplicatedGrandchildren->size() == 1);
		if (DuplicatedGrandchildren && !DuplicatedGrandchildren->empty())
			PF_CHECK(Tests, DuplicatedGrandchildren->front().GetTag()->Name == "Grandchild Copy");

		const auto RepeatedDuplicate = EditorSceneHierarchyOperations::DuplicateSubtrees(SceneValue, DuplicationRoots);
		PF_CHECK(Tests, RepeatedDuplicate && RepeatedDuplicate->size() == 1);
		PF_CHECK(Tests, SceneValue.GetEntityCount() == 10);
		if (RepeatedDuplicate && !RepeatedDuplicate->empty())
		{
			const auto RepeatedRoot = SceneValue.FindEntity(RepeatedDuplicate->front());
			PF_CHECK(Tests, RepeatedRoot && RepeatedRoot->GetTag()->Name == "Root Copy 2");
			if (RepeatedRoot)
			{
				const auto RepeatedChildren = RepeatedRoot->GetChildren();
				PF_CHECK(Tests, RepeatedChildren && RepeatedChildren->size() == 1);
				if (RepeatedChildren && !RepeatedChildren->empty())
					PF_CHECK(Tests, RepeatedChildren->front().GetTag()->Name == "Child Copy 2");
			}
		}
	}

	void TestEditorHierarchySearchPolicy(TestRunner& Tests)
	{
		using namespace PulseForge;
		using namespace PulseForgeEditor;
		Scene SceneValue;
		auto Root = SceneValue.CreateEntity("Material props");
		auto Child = SceneValue.CreateEntity("Packed Metal");
		auto Grandchild = SceneValue.CreateEntity("Nested mesh");
		auto Separate = SceneValue.CreateEntity("Camera rig");
		PF_CHECK(Tests, Root && Child && Grandchild && Separate);
		if (!Root || !Child || !Grandchild || !Separate)
			return;
		PF_CHECK(Tests, Child->SetParent(*Root));
		PF_CHECK(Tests, Grandchild->SetParent(*Child));

		const auto Visible = GetVisibleHierarchyEntities(SceneValue, "pAcKeD");
		PF_CHECK(Tests, Visible.contains(Root->GetUUID()));
		PF_CHECK(Tests, Visible.contains(Child->GetUUID()));
		PF_CHECK(Tests, !Visible.contains(Grandchild->GetUUID()));
		PF_CHECK(Tests, !Visible.contains(Separate->GetUUID()));
		PF_CHECK(Tests, GetVisibleHierarchyEntities(SceneValue, "missing").empty());
		PF_CHECK(Tests, GetVisibleHierarchyEntities(SceneValue, "").empty());
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

	void TestEditorSceneWorkspaceState(TestRunner& Tests)
	{
		using namespace PulseForgeEditor;
		const auto First = PulseForge::UUID::Generate();
		const auto Second = PulseForge::UUID::Generate();
		const auto Third = PulseForge::UUID::Generate();
		PF_CHECK(Tests, First.has_value() && Second.has_value() && Third.has_value());
		if (!First || !Second || !Third)
			return;

		EditorSceneWorkspaceState State;
		State.BeginHierarchyFrame();
		State.RecordVisibleHierarchyEntity(*First);
		State.RecordVisibleHierarchyEntity(*Second);
		PF_CHECK(Tests, State.VisibleHierarchyOrder().empty());
		State.CompleteHierarchyFrame();
		PF_CHECK(Tests, State.VisibleHierarchyOrder().size() == 2);
		PF_CHECK(Tests, State.VisibleHierarchyOrder()[0] == *First);
		PF_CHECK(Tests, State.VisibleHierarchyOrder()[1] == *Second);

		State.Selection().SelectSingle(*Second);
		State.BeginHierarchyFrame();
		State.RecordVisibleHierarchyEntity(*Second);
		State.RecordVisibleHierarchyEntity(*Third);
		PF_CHECK(Tests, State.VisibleHierarchyOrder()[0] == *First);
		State.CompleteHierarchyFrame();
		PF_CHECK(Tests, State.Selection().SelectRange(State.VisibleHierarchyOrder(), *Third));
		PF_CHECK(Tests, State.Selection().GetEntities().size() == 2);
		PF_CHECK(Tests, State.Selection().GetEntities()[0] == *Second);
		PF_CHECK(Tests, State.Selection().GetEntities()[1] == *Third);

		State.Visibility().SetVisible(*First, false);
		PF_CHECK(Tests, !State.Visibility().IsLocallyVisible(*First));
		State.Reset();
		PF_CHECK(Tests, State.Selection().GetEntities().empty());
		PF_CHECK(Tests, State.Visibility().IsLocallyVisible(*First));
		PF_CHECK(Tests, State.VisibleHierarchyOrder().empty());
	}

	void TestEditorSceneHistoryState(TestRunner& Tests)
	{
		using namespace PulseForgeEditor;
		PulseForge::Scene SceneValue;
		const auto Original = SceneValue.CreateEntity("Original");
		PF_CHECK(Tests, Original.has_value());
		if (!Original)
			return;

		EditorSceneWorkspaceState Workspace;
		Workspace.Selection().SelectSingle(Original->GetUUID());
		Workspace.Visibility().SetVisible(Original->GetUUID(), false);
		const auto Snapshot = EditorSceneHistoryState::CaptureSnapshot(SceneValue, Workspace);
		PF_CHECK(Tests, Snapshot.has_value());
		if (!Snapshot)
			return;

		EditorSceneHistoryState History;
		History.Reset(&SceneValue);
		PF_CHECK(Tests, !History.IsDirty());
		const auto Additional = SceneValue.CreateEntity("Additional");
		PF_CHECK(Tests, Additional.has_value());
		if (!Additional)
			return;
		Workspace.Selection().SelectSingle(Additional->GetUUID());
		Workspace.Visibility().SetVisible(Original->GetUUID(), true);
		History.UpdateDirty(&SceneValue);
		PF_CHECK(Tests, History.IsDirty());

		PF_CHECK(Tests, EditorSceneHistoryState::RestoreSnapshot(*Snapshot, SceneValue, Workspace).has_value());
		PF_CHECK(Tests, SceneValue.GetEntityCount() == 1);
		PF_CHECK(Tests, Workspace.Selection().GetPrimary() == Original->GetUUID());
		PF_CHECK(Tests, !Workspace.Visibility().IsLocallyVisible(Original->GetUUID()));

		const auto Invalid = EditorSceneHistoryState::RestoreSnapshot("invalid snapshot", SceneValue, Workspace);
		PF_CHECK(Tests, !Invalid);
		PF_CHECK(Tests, SceneValue.GetEntityCount() == 1);
		PF_CHECK(Tests, Workspace.Selection().GetPrimary() == Original->GetUUID());

		const auto Changed = SceneValue.CreateEntity("Changed");
		PF_CHECK(Tests, Changed.has_value());
		History.UpdateDirty(&SceneValue);
		PF_CHECK(Tests, History.IsDirty());
		History.MarkSaved(&SceneValue);
		PF_CHECK(Tests, !History.IsDirty());
		History.Reset(nullptr);
		PF_CHECK(Tests, !History.IsDirty());
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
}

void RunEditorStateTests(TestRunner& Tests)
{
	TestEditorConsoleHistory(Tests);
	TestEditorContentBrowserState(Tests);
	TestEditorEntityClipboard(Tests);
	TestEditorStatusPanel(Tests);
	TestEditorSceneHierarchyOperations(Tests);
	TestEditorHierarchySearchPolicy(Tests);
	TestEditorAssetWorkflowState(Tests);
	TestEditorSceneWorkspaceState(Tests);
	TestEditorSceneHistoryState(Tests);
	TestEditorMaterialDraftStore(Tests);
}
