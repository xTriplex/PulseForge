#include "TestEditorSupport.h"
#include "nlohmann/json.hpp"
#include "Editor/EditorEntityClipboard.h"
#include "Editor/EditorHierarchyPanelPolicy.h"
#include "Editor/EditorSceneEntityCommands.h"
#include "Editor/EditorSceneTransitionQueue.h"
#include "Editor/EditorSceneVisibilityCommands.h"

namespace
{
	void TestEditorSceneTransitionQueue(TestRunner& Tests)
	{
		using namespace PulseForgeEditor;
		EditorSceneTransitionQueue Queue;
		size_t Continuations = 0;
		auto Ready = Queue.Request(false, [&] { ++Continuations; });
		PF_CHECK(Tests, Ready.Status == EditorSceneTransitionQueue::RequestStatus::Ready);
		PF_CHECK(Tests, static_cast<bool>(Ready.ReadyAction));
		if (Ready.ReadyAction) Ready.ReadyAction();
		PF_CHECK(Tests, Continuations == 1 && !Queue.HasPendingDecision());

		auto Pending = Queue.Request(true, [&] { ++Continuations; });
		PF_CHECK(Tests, Pending.Status == EditorSceneTransitionQueue::RequestStatus::AwaitingDecision);
		PF_CHECK(Tests, Queue.HasPendingDecision());
		auto Rejected = Queue.Request(false, [&] { ++Continuations; });
		PF_CHECK(Tests, Rejected.Status == EditorSceneTransitionQueue::RequestStatus::Rejected);
		PF_CHECK(Tests, !Queue.Resolve(false));
		PF_CHECK(Tests, Queue.HasPendingDecision() && Continuations == 1);
		auto Resolved = Queue.Resolve(true);
		PF_CHECK(Tests, Resolved.has_value() && !Queue.HasPendingDecision());
		if (Resolved && *Resolved) (*Resolved)();
		PF_CHECK(Tests, Continuations == 2);
		PF_CHECK(Tests, !Queue.Resolve(true));

		Pending = Queue.Request(true, [&] { ++Continuations; });
		PF_CHECK(Tests, Pending.Status == EditorSceneTransitionQueue::RequestStatus::AwaitingDecision);
		Queue.Cancel();
		PF_CHECK(Tests, !Queue.HasPendingDecision() && !Queue.Resolve(true));
		PF_CHECK(Tests, Continuations == 2);
		PF_CHECK(Tests, Queue.Request(false, {}).Status == EditorSceneTransitionQueue::RequestStatus::Rejected);
	}

	class TestEditorSceneVisibilityActions final : public PulseForgeEditor::EditorSceneCommandActions
	{
	public:
		[[nodiscard]] std::expected<void, std::string> RunEditorSceneEdit(
			std::string Description, std::function<std::expected<void, PulseForge::SceneError>()> Operation) override
		{
			++EditCount;
			EditDescriptions.push_back(std::move(Description));
			if (FailNextEdit)
			{
				FailNextEdit = false;
				return std::unexpected("injected visibility edit failure");
			}
			auto Result = Operation();
			if (!Result) return std::unexpected(Result.error().Message);
			return {};
		}

		void ReportEditorSceneCommandError(std::string Message) override { Errors.push_back(std::move(Message)); }
		void SetEditorSceneCommandStatus(std::string Message) override { Statuses.push_back(std::move(Message)); }

		size_t EditCount = 0;
		bool FailNextEdit = false;
		std::vector<std::string> EditDescriptions;
		std::vector<std::string> Errors;
		std::vector<std::string> Statuses;
	};

	class TestEditorSceneEntityCommandActions final : public PulseForgeEditor::EditorSceneEntityCommandActions
	{
	public:
		explicit TestEditorSceneEntityCommandActions(PulseForgeEditor::EditorSceneWorkspaceState& Workspace) : m_Workspace(Workspace) {}

		[[nodiscard]] std::expected<void, std::string> RunEditorSceneEdit(
			std::string Description, std::function<std::expected<void, PulseForge::SceneError>()> Operation) override
		{
			++EditCount;
			EditDescriptions.push_back(std::move(Description));
			if (FailNextEdit)
			{
				FailNextEdit = false;
				return std::unexpected("injected scene command failure");
			}
			auto Result = Operation();
			if (!Result) return std::unexpected(Result.error().Message);
			return {};
		}

		void ReportEditorSceneCommandError(std::string Message) override { Errors.push_back(std::move(Message)); }
		void SetEditorSceneCommandStatus(std::string Message) override { Statuses.push_back(std::move(Message)); }
		void ReportEditorSceneCommandWarning(std::string Message) override { Warnings.push_back(std::move(Message)); }
		void SetEditorSceneSelection(std::vector<PulseForge::UUID> Identifiers) override
		{
			m_Workspace.Selection().Set(std::move(Identifiers));
		}
		void SelectSingleEditorEntity(std::optional<PulseForge::UUID> Identifier) override
		{
			if (Identifier) m_Workspace.Selection().SelectSingle(*Identifier);
			else m_Workspace.Selection().Clear();
		}

		PulseForgeEditor::EditorSceneWorkspaceState& m_Workspace;
		size_t EditCount = 0;
		bool FailNextEdit = false;
		std::vector<std::string> EditDescriptions;
		std::vector<std::string> Errors;
		std::vector<std::string> Statuses;
		std::vector<std::string> Warnings;
	};

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
			{ "entities", Json::array({ Json{ { "uuid", Root.ToString() }, { "tag", Json{ { "name", "Root" } } },
											{ "camera", Json{ { "primary", true } } } },
							  Json{ { "uuid", Child.ToString() }, { "tag", Json{ { "name", "Child" } } }, { "parent", Root.ToString() },
								  { "audioListener", Json{ { "primary", true } } } },
							  Json{ { "uuid", Other.ToString() }, { "tag", Json{ { "name", "Other" } } } } }) }
		}.dump();
		const std::array Roots{ Root };
		const std::array Included{ Root, Child };
		auto ClipboardResult = EditorEntityClipboard::Capture(Project, SourceScene, Roots, Included, Source);
		PF_CHECK(Tests, ClipboardResult.has_value());
		if (!ClipboardResult) return;
		EditorEntityClipboard Clipboard = std::move(*ClipboardResult);
		PF_CHECK(Tests, Clipboard.HasData());
		PF_CHECK(Tests, Clipboard.CanPaste(Project));
		PF_CHECK(Tests, !Clipboard.CanPaste(UUID{ 12, 1 }));

		const UUID Existing{ 77, 7 };
		const UUID NewRoot{ 88, 8 };
		const UUID NewChild{ 99, 9 };
		const std::string Destination = Json{
			{ "entities", Json::array({ Json{ { "uuid", Existing.ToString() }, { "tag", Json{ { "name", "Root Copy" } } } } }) }
		}.dump();
		const std::array Generated{ Existing, NewRoot, NewChild };
		size_t GeneratedIndex = 0;
		auto Prepared = Clipboard.PreparePaste(
			Project, SourceScene, Destination, {}, [&]() -> std::expected<UUID, std::string> { return Generated[GeneratedIndex++]; });
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
		PF_CHECK(Tests, Clipboard
							.PreparePaste(Project, AssetID{ 23, 2 }, Destination, {},
								[&]() -> std::expected<UUID, std::string> { return Generated[CrossSceneIdentifierIndex++ + 1]; })
							.has_value());
		PF_CHECK(Tests, !Clipboard.PreparePaste(UUID{ 12, 1 }, SourceScene, Destination, {}).has_value());
		PF_CHECK(Tests, !Clipboard
							.PreparePaste(Project, SourceScene, Destination, {},
								[]() -> std::expected<UUID, std::string> { return std::unexpected("identifier generation failed"); })
							.has_value());

		const std::string ExternalSource = Json{
			{ "entities", Json::array({ Json{ { "uuid", ExternalParent.ToString() }, { "tag", Json{ { "name", "Parent" } } } },
							  Json{ { "uuid", Other.ToString() }, { "tag", Json{ { "name", "Child" } } },
								  { "parent", ExternalParent.ToString() } } }) }
		}.dump();
		const std::array ExternalRoots{ Other };
		const std::array ExternalIncluded{ Other };
		auto ExternalClipboard = EditorEntityClipboard::Capture(Project, SourceScene, ExternalRoots, ExternalIncluded, ExternalSource);
		PF_CHECK(Tests, ExternalClipboard.has_value());
		if (ExternalClipboard)
		{
			const std::string TargetWithoutParent = Json{ { "entities", Json::array() } }.dump();
			PF_CHECK(Tests, !ExternalClipboard->PreparePaste(Project, AssetID{ 23, 2 }, TargetWithoutParent, {}).has_value());
			const std::string TargetWithParent =
				Json{ { "entities", Json::array({ Json{ { "uuid", ExternalParent.ToString() } } }) } }.dump();
			PF_CHECK(Tests, ExternalClipboard->PreparePaste(Project, SourceScene, TargetWithParent, {}).has_value());
		}

		const std::string AssetSource = Json{
			{ "entities", Json::array({ Json{
							  { "uuid", Other.ToString() }, { "meshRenderer", Json{ { "meshAsset", UUID{ 101, 10 }.ToString() } } } } }) }
		}.dump();
		const std::array AssetRoots{ Other };
		auto AssetClipboard = EditorEntityClipboard::Capture(Project, SourceScene, AssetRoots, AssetRoots, AssetSource);
		PF_CHECK(Tests, AssetClipboard.has_value());
		if (AssetClipboard)
		{
			const std::string EmptyScene = Json{ { "entities", Json::array() } }.dump();
			PF_CHECK(Tests, !AssetClipboard->PreparePaste(Project, SourceScene, EmptyScene, [](const UUID&) { return false; }).has_value());
			PF_CHECK(Tests, AssetClipboard->PreparePaste(Project, SourceScene, EmptyScene, [](const UUID&) { return true; }).has_value());
		}
		PF_CHECK(Tests, !EditorEntityClipboard::Capture(Project, SourceScene, Roots, Included, "not-json").has_value());
	}

	void TestEditorSceneVisibilityCommands(TestRunner& Tests)
	{
		using namespace PulseForge;
		using namespace PulseForgeEditor;
		Scene SceneValue;
		auto Root = SceneValue.CreateEntity("Root");
		auto Child = SceneValue.CreateEntity("Child");
		auto Grandchild = SceneValue.CreateEntity("Grandchild");
		auto Separate = SceneValue.CreateEntity("Separate");
		PF_CHECK(Tests, Root && Child && Grandchild && Separate);
		if (!Root || !Child || !Grandchild || !Separate) return;
		PF_CHECK(Tests, Child->SetParent(*Root));
		PF_CHECK(Tests, Grandchild->SetParent(*Child));

		EditorSceneWorkspaceState Workspace;
		Workspace.Selection().SelectSingle(Child->GetUUID());
		EditorSceneVisibilityCommands Commands;
		TestEditorSceneVisibilityActions Actions;
		Commands.HideSelectedEntities(&SceneValue, Workspace, false, Actions);
		PF_CHECK(Tests, !Workspace.Visibility().IsLocallyVisible(Child->GetUUID()));
		PF_CHECK(Tests, Actions.EditCount == 1 && Actions.EditDescriptions.back() == "Hide Selected");
		PF_CHECK(Tests, Actions.Statuses.size() == 1);

		const auto Hidden = EditorSceneVisibilityCommands::GetEffectivelyHiddenEntities(&SceneValue, Workspace.Visibility());
		PF_CHECK(Tests, Hidden.size() == 2);
		PF_CHECK(Tests, std::find(Hidden.begin(), Hidden.end(), Child->GetUUID()) != Hidden.end());
		PF_CHECK(Tests, std::find(Hidden.begin(), Hidden.end(), Grandchild->GetUUID()) != Hidden.end());

		Commands.ToggleEntityVisibility(&SceneValue, Workspace, Child->GetUUID(), false, Actions);
		PF_CHECK(Tests, Workspace.Visibility().IsLocallyVisible(Child->GetUUID()));
		PF_CHECK(Tests, Actions.EditCount == 2 && Actions.EditDescriptions.back() == "Show Entity");

		Commands.IsolateSelectedEntities(&SceneValue, Workspace, false, Actions);
		PF_CHECK(Tests, Workspace.Visibility().IsLocallyVisible(Root->GetUUID()));
		PF_CHECK(Tests, Workspace.Visibility().IsLocallyVisible(Child->GetUUID()));
		PF_CHECK(Tests, Workspace.Visibility().IsLocallyVisible(Grandchild->GetUUID()));
		PF_CHECK(Tests, !Workspace.Visibility().IsLocallyVisible(Separate->GetUUID()));
		PF_CHECK(Tests, Workspace.Visibility().GetIsolationBaseline().has_value());

		Commands.RestoreIsolatedVisibility(&SceneValue, Workspace, false, Actions);
		PF_CHECK(Tests, Workspace.Visibility().GetHiddenEntities().empty());
		PF_CHECK(Tests, !Workspace.Visibility().GetIsolationBaseline());

		Actions.FailNextEdit = true;
		Commands.HideSelectedEntities(&SceneValue, Workspace, false, Actions);
		PF_CHECK(Tests, Workspace.Visibility().IsLocallyVisible(Child->GetUUID()));
		PF_CHECK(Tests, Actions.Errors.size() == 1 && Actions.Errors.front().find("injected visibility edit failure") != std::string::npos);

		const size_t EditsBeforeRuntime = Actions.EditCount;
		Commands.HideSelectedEntities(&SceneValue, Workspace, true, Actions);
		Commands.ToggleEntityVisibility(&SceneValue, Workspace, Child->GetUUID(), true, Actions);
		PF_CHECK(Tests, Actions.EditCount == EditsBeforeRuntime);
		PF_CHECK(Tests, EditorSceneVisibilityCommands::GetEffectivelyHiddenEntities(nullptr, Workspace.Visibility()).empty());
	}

	void TestEditorSceneEntityCommands(TestRunner& Tests)
	{
		using namespace PulseForge;
		using namespace PulseForgeEditor;
		Scene SceneValue;
		auto Root = SceneValue.CreateEntity("Root");
		auto Child = SceneValue.CreateEntity("Child");
		auto Separate = SceneValue.CreateEntity("Separate");
		PF_CHECK(Tests, Root && Child && Separate);
		if (!Root || !Child || !Separate) return;
		PF_CHECK(Tests, Child->SetParent(*Root));
		const UUID RootID = Root->GetUUID();
		const UUID ChildID = Child->GetUUID();
		const UUID SeparateID = Separate->GetUUID();

		const UUID ProjectID{ 41, 1 };
		const AssetID SceneAsset{ 42, 2 };
		EditorSceneWorkspaceState Workspace;
		TestEditorSceneEntityCommandActions Actions(Workspace);
		EditorSceneEntityCommands Commands;

		Workspace.Selection().Set({ RootID, ChildID });
		Commands.DuplicateSelectedEntities(&SceneValue, Workspace, Actions);
		PF_CHECK(Tests, SceneValue.GetEntityCount() == 5);
		PF_CHECK(Tests, Workspace.Selection().GetEntities().size() == 1);
		PF_CHECK(Tests, Actions.EditDescriptions.back() == "Duplicate Entities");
		const auto DuplicatedRoot = SceneValue.FindEntity(Workspace.Selection().GetEntities().front());
		PF_CHECK(Tests, DuplicatedRoot.has_value());
		if (DuplicatedRoot)
		{
			const auto DuplicatedChildren = DuplicatedRoot->GetChildren();
			PF_CHECK(Tests, DuplicatedChildren && DuplicatedChildren->size() == 1);
		}

		Commands.CreateEntity(&SceneValue, Actions);
		PF_CHECK(Tests, SceneValue.GetEntityCount() == 6);
		PF_CHECK(Tests, Workspace.Selection().GetEntities().size() == 1);
		PF_CHECK(Tests, Actions.EditDescriptions.back() == "Create Entity");

		Workspace.Selection().SelectSingle(RootID);
		PF_CHECK(Tests, Commands.CopySelectedEntities(&SceneValue, ProjectID, SceneAsset, Workspace, Actions));
		PF_CHECK(Tests, Commands.CanPaste(ProjectID));
		const size_t BeforePaste = SceneValue.GetEntityCount();
		Commands.PasteEntities(&SceneValue, ProjectID, SceneAsset, Workspace, false, [](const UUID&) { return true; }, Actions);
		PF_CHECK(Tests, SceneValue.GetEntityCount() == BeforePaste + 2);
		PF_CHECK(Tests, Workspace.Selection().GetEntities().size() == 1);
		PF_CHECK(Tests, Actions.EditDescriptions.back() == "Paste Entities");

		const size_t BeforeRejectedPaste = SceneValue.GetEntityCount();
		Commands.PasteEntities(&SceneValue, UUID{ 99, 9 }, SceneAsset, Workspace, false, [](const UUID&) { return true; }, Actions);
		PF_CHECK(Tests, SceneValue.GetEntityCount() == BeforeRejectedPaste);
		PF_CHECK(Tests, Actions.Warnings.size() == 1);
		Commands.PasteEntities(&SceneValue, ProjectID, SceneAsset, Workspace, true, [](const UUID&) { return true; }, Actions);
		PF_CHECK(Tests, SceneValue.GetEntityCount() == BeforeRejectedPaste);

		const auto ChildAfterPaste = SceneValue.FindEntity(ChildID);
		const auto SeparateAfterPaste = SceneValue.FindEntity(SeparateID);
		PF_CHECK(Tests, ChildAfterPaste && SeparateAfterPaste);
		if (!ChildAfterPaste || !SeparateAfterPaste) return;
		Workspace.Selection().SelectSingle(ChildID);
		Commands.ReparentEntity(&SceneValue, ChildID, SeparateID, Actions);
		auto ReparentedChild = SceneValue.FindEntity(ChildID);
		PF_CHECK(Tests, ReparentedChild.has_value());
		if (!ReparentedChild) return;
		const auto ReparentedParent = ReparentedChild->GetParent();
		PF_CHECK(Tests, ReparentedParent && ReparentedParent->has_value() && (**ReparentedParent).GetUUID() == SeparateID);
		Commands.ReparentEntity(&SceneValue, ChildID, std::nullopt, Actions);
		ReparentedChild = SceneValue.FindEntity(ChildID);
		PF_CHECK(Tests, ReparentedChild.has_value());
		if (!ReparentedChild) return;
		const auto ClearedParent = ReparentedChild->GetParent();
		PF_CHECK(Tests, ClearedParent && !ClearedParent->has_value());
		const auto RootAfterPaste = SceneValue.FindEntity(RootID);
		PF_CHECK(Tests, RootAfterPaste.has_value());
		if (!RootAfterPaste) return;
		PF_CHECK(Tests, ReparentedChild->SetParent(*RootAfterPaste));
		const size_t BeforeCut = SceneValue.GetEntityCount();
		Workspace.Selection().SelectSingle(SeparateID);
		Commands.CutSelectedEntities(&SceneValue, ProjectID, SceneAsset, Workspace, Actions);
		PF_CHECK(Tests, SceneValue.GetEntityCount() == BeforeCut - 1);
		PF_CHECK(Tests, !SceneValue.FindEntity(SeparateID));

		Workspace.Visibility().SetVisible(RootID, false);
		Workspace.Selection().SelectSingle(RootID);
		const size_t BeforeDelete = SceneValue.GetEntityCount();
		Commands.DeleteSelectedEntities(&SceneValue, Workspace, Actions);
		PF_CHECK(Tests, SceneValue.GetEntityCount() == BeforeDelete - 2);
		PF_CHECK(Tests, Workspace.Selection().GetEntities().empty());
		PF_CHECK(Tests, Workspace.Visibility().IsLocallyVisible(RootID));

		const size_t BeforeFailedCreate = SceneValue.GetEntityCount();
		Actions.FailNextEdit = true;
		Commands.CreateEntity(&SceneValue, Actions);
		PF_CHECK(Tests, SceneValue.GetEntityCount() == BeforeFailedCreate);
		PF_CHECK(Tests, !Actions.Errors.empty());
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
		if (!Root || !Child || !Grandchild || !Separate) return;
		PF_CHECK(Tests, Child->SetParent(*Root));
		PF_CHECK(Tests, Grandchild->SetParent(*Child));

		const std::array Selected{ Root->GetUUID(), Child->GetUUID(), Separate->GetUUID() };
		const auto Roots = EditorSceneHierarchyOperations::GetTopLevelSelection(SceneValue, Selected);
		PF_CHECK(Tests, Roots && Roots->size() == 2);
		if (!Roots) return;
		PF_CHECK(Tests, (*Roots)[0].GetUUID() == Root->GetUUID());
		PF_CHECK(Tests, (*Roots)[1].GetUUID() == Separate->GetUUID());
		const auto Subtree = EditorSceneHierarchyOperations::CollectSubtrees(std::span<const Entity>(&(*Roots)[0], 1));
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
		if (!Duplicated || Duplicated->empty()) return;
		const auto DuplicatedRoot = SceneValue.FindEntity(Duplicated->front());
		PF_CHECK(Tests, DuplicatedRoot.has_value());
		if (!DuplicatedRoot) return;
		PF_CHECK(Tests, DuplicatedRoot->GetTag()->Name == "Root Copy");
		const auto DuplicatedChildren = DuplicatedRoot->GetChildren();
		PF_CHECK(Tests, DuplicatedChildren && DuplicatedChildren->size() == 1);
		if (!DuplicatedChildren || DuplicatedChildren->empty()) return;
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
		if (!Root || !Child || !Grandchild || !Separate) return;
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

	void TestEditorSceneWorkspaceState(TestRunner& Tests)
	{
		using namespace PulseForgeEditor;
		const auto First = PulseForge::UUID::Generate();
		const auto Second = PulseForge::UUID::Generate();
		const auto Third = PulseForge::UUID::Generate();
		PF_CHECK(Tests, First.has_value() && Second.has_value() && Third.has_value());
		if (!First || !Second || !Third) return;

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
		if (!Original) return;

		EditorSceneWorkspaceState Workspace;
		Workspace.Selection().SelectSingle(Original->GetUUID());
		Workspace.Visibility().SetVisible(Original->GetUUID(), false);
		const auto Snapshot = EditorSceneHistoryState::CaptureSnapshot(SceneValue, Workspace);
		PF_CHECK(Tests, Snapshot.has_value());
		if (!Snapshot) return;

		EditorSceneHistoryState History;
		History.Reset(&SceneValue);
		PF_CHECK(Tests, !History.IsDirty());
		const auto Additional = SceneValue.CreateEntity("Additional");
		PF_CHECK(Tests, Additional.has_value());
		if (!Additional) return;
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
} // namespace

void RunEditorSceneStateTests(TestRunner& Tests)
{
	TestEditorSceneTransitionQueue(Tests);
	TestEditorEntityClipboard(Tests);
	TestEditorSceneVisibilityCommands(Tests);
	TestEditorSceneEntityCommands(Tests);
	TestEditorSceneHierarchyOperations(Tests);
	TestEditorHierarchySearchPolicy(Tests);
	TestEditorSceneWorkspaceState(Tests);
	TestEditorSceneHistoryState(Tests);
}
