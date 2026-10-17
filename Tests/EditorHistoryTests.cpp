#include "TestEditorSupport.h"
#include "Editor/EditorSceneEditController.h"

namespace
{
	void TestEditorCommandHistory(TestRunner& Tests)
	{
		using PulseForgeEditor::EditorCommandHistory;
		EditorCommandHistory History(2, 1024);
		History.Reset();
		PF_CHECK(Tests, !History.IsDirty() && !History.CanUndo() && !History.CanRedo());

		std::string Current = "scene-0";
		PF_CHECK(Tests, History.Begin("Create entity", Current));
		Current = "scene-1";
		const auto FirstCommit = History.Commit(Current);
		PF_CHECK(Tests, FirstCommit && *FirstCommit && History.IsDirty() && History.CanUndo());
		PF_CHECK(Tests, History.GetUndoCommand() && History.GetUndoCommand()->Description == "Create entity");

		auto Restore = [&Current](std::string_view State) -> std::expected<void, std::string>
		{
			Current.assign(State);
			return {};
		};
		const auto Undone = History.Undo(Restore);
		PF_CHECK(Tests, Undone && *Undone && Current == "scene-0" && !History.IsDirty());
		const auto Redone = History.Redo(Restore);
		PF_CHECK(Tests, Redone && *Redone && Current == "scene-1" && History.IsDirty());

		History.MarkSaved();
		PF_CHECK(Tests, !History.IsDirty());
		PF_CHECK(Tests, History.Undo(Restore) && History.IsDirty());
		PF_CHECK(Tests, History.Redo(Restore) && !History.IsDirty());

		PF_CHECK(Tests, History.Undo(Restore));
		PF_CHECK(Tests, History.Begin("Rename entity", Current));
		Current = "renamed";
		PF_CHECK(Tests, History.Commit(Current));
		PF_CHECK(Tests, !History.CanRedo() && Current == "renamed");

		const size_t BeforeNoOp = History.GetCommandCount();
		PF_CHECK(Tests, History.Begin("No-op", Current));
		const auto NoOp = History.Commit(Current);
		PF_CHECK(Tests, NoOp && !*NoOp && History.GetCommandCount() == BeforeNoOp);

		PF_CHECK(Tests, History.Begin("Third command", Current));
		Current = "third";
		PF_CHECK(Tests, History.Commit(Current));
		PF_CHECK(Tests, History.GetCommandCount() == 2 && History.GetStoredBytes() <= History.GetMaximumBytes());
		PF_CHECK(Tests, History.Undo(Restore) && Current == "renamed");
		PF_CHECK(Tests, History.Undo(Restore));
		PF_CHECK(Tests, !History.CanUndo());

		EditorCommandHistory ByteLimited(4, 8);
		ByteLimited.Reset();
		PF_CHECK(Tests, ByteLimited.Begin("Large command", "1234"));
		const auto Rejected = ByteLimited.Commit("12345678");
		PF_CHECK(Tests, !Rejected && ByteLimited.IsActive() && !ByteLimited.CanUndo());
		Current = "12345678";
		const auto Cancelled = ByteLimited.Cancel(
			[&Current](std::string_view State) -> std::expected<void, std::string>
			{
				Current.assign(State);
				return {};
			});
		PF_CHECK(Tests, Cancelled && Current == "1234" && !ByteLimited.IsActive());

		PF_CHECK(Tests, ByteLimited.Begin("Failed restore", "a"));
		Current = "b";
		PF_CHECK(Tests, ByteLimited.Commit(Current));
		const auto FailedUndo =
			ByteLimited.Undo([](std::string_view) -> std::expected<void, std::string> { return std::unexpected("restore rejected"); });
		PF_CHECK(Tests, !FailedUndo && ByteLimited.CanUndo() && Current == "b");
	}

	void TestAltGizmoDragHistoryIntegration(TestRunner& Tests)
	{
		using namespace PulseForge;
		using namespace PulseForgeEditor;

		const auto Capture = [](const Scene& SceneValue, const EditorSelection& Selection) -> std::expected<std::string, std::string>
		{
			auto SerializedScene = SceneSerializer::Serialize(SceneValue);
			if (!SerializedScene) return std::unexpected(SerializedScene.error().Message);
			try
			{
				nlohmann::json State;
				State["scene"] = nlohmann::json::parse(*SerializedScene);
				State["selection"] = nlohmann::json::array();
				for (const UUID& Identifier : Selection.GetEntities())
					State["selection"].push_back(Identifier.ToString());
				State["primary"] = Selection.GetPrimary() ? Selection.GetPrimary()->ToString() : std::string{};
				return State.dump();
			}
			catch (const std::exception& Error)
			{
				return std::unexpected(Error.what());
			}
		};
		const auto Restore = [](std::string_view StateData, Scene& SceneValue,
								 EditorSelection& Selection) -> std::expected<void, std::string>
		{
			try
			{
				const nlohmann::json State = nlohmann::json::parse(StateData);
				if (auto Result = SceneSerializer::Deserialize(State.at("scene").dump(), SceneValue); !Result)
					return std::unexpected(Result.error().Message);

				std::vector<UUID> Identifiers;
				for (const auto& SerializedIdentifier : State.at("selection"))
				{
					auto Identifier = UUID::Parse(SerializedIdentifier.get<std::string>());
					if (!Identifier) return std::unexpected(Identifier.error().Message);
					if (SceneValue.FindEntity(*Identifier)) Identifiers.push_back(*Identifier);
				}
				std::optional<UUID> Primary;
				const std::string SerializedPrimary = State.at("primary").get<std::string>();
				if (!SerializedPrimary.empty())
				{
					auto Identifier = UUID::Parse(SerializedPrimary);
					if (!Identifier) return std::unexpected(Identifier.error().Message);
					if (SceneValue.FindEntity(*Identifier)) Primary = *Identifier;
				}
				Selection.Set(std::move(Identifiers), Primary);
				return {};
			}
			catch (const std::exception& Error)
			{
				return std::unexpected(Error.what());
			}
		};

		const auto RunSingleGesture = [&](TransformGizmoOperation Operation, bool Duplicate, std::string Description)
		{
			Scene SceneValue;
			EditorSelection Selection;
			EditorCommandHistory History;
			History.Reset();
			auto Original = SceneValue.CreateEntity("Original");
			PF_CHECK(Tests, Original.has_value());
			if (!Original) return;
			TransformComponent Initial;
			Initial.Translation = { 1.0f, 2.0f, 3.0f };
			Initial.Scale = { 1.5f, 0.8f, 1.2f };
			PF_CHECK(Tests, Original->SetTransform(Initial));
			const UUID OriginalID = Original->GetUUID();
			Selection.SelectSingle(OriginalID);
			auto Before = Capture(SceneValue, Selection);
			PF_CHECK(Tests, Before.has_value());
			if (!Before || !History.Begin(Description, *Before)) return;

			// The viewport is drawn before the Inspector. The production Inspector policy must leave
			// a gizmo-owned transaction active while the gesture is still changing scene state.
			const uint32_t GizmoInspectorItem = NoInspectorHistoryItem;
			const bool PrematurelyFinished = ShouldFinishSceneEditForInspector(History.IsActive(), GizmoInspectorItem, 0);
			PF_CHECK(Tests, !PrematurelyFinished && History.IsActive() && !History.CanUndo());
			if (PrematurelyFinished)
			{
				auto InspectorBoundary = Capture(SceneValue, Selection);
				if (InspectorBoundary) (void)History.Commit(std::move(*InspectorBoundary));
			}

			PF_CHECK(Tests, GetViewportGizmoPointerOutcome({ true, true, false }, false) == ViewportGizmoPointerOutcome::Continue);
			std::optional<UUID> DuplicateID;
			std::optional<Entity> Target;
			if (Duplicate)
			{
				auto Duplicated = SceneValue.DuplicateEntity(*Original);
				PF_CHECK(Tests, Duplicated.has_value());
				if (!Duplicated)
				{
					(void)History.Cancel([&](std::string_view State) { return Restore(State, SceneValue, Selection); });
					return;
				}
				DuplicateID = Duplicated->GetUUID();
				Selection.SelectSingle(*DuplicateID);
				Target = *Duplicated;
			}
			else Target = *Original;

			TransformComponent Final = Initial;
			if (Operation == TransformGizmoOperation::Translate) Final.Translation += glm::vec3(2.5f, -0.25f, 1.0f);
			else if (Operation == TransformGizmoOperation::Rotate)
				Final.Rotation = glm::normalize(glm::angleAxis(glm::radians(37.0f), glm::vec3(0.0f, 1.0f, 0.0f)) * Initial.Rotation);
			else Final.Scale *= glm::vec3(1.25f, 0.75f, 1.5f);
			PF_CHECK(Tests, Target->SetTransform(Final));
			PF_CHECK(Tests, !ShouldFinishSceneEditForInspector(History.IsActive(), GizmoInspectorItem, 0));
			PF_CHECK(Tests, History.IsActive() && !History.CanUndo());

			const auto Release = GetViewportGizmoPointerOutcome({ true, false, true }, false);
			PF_CHECK(Tests, Release == ViewportGizmoPointerOutcome::Complete);
			if (Release != ViewportGizmoPointerOutcome::Complete) return;
			auto After = Capture(SceneValue, Selection);
			PF_CHECK(Tests, After.has_value());
			if (!After) return;
			const auto Committed = History.Commit(std::move(*After));
			PF_CHECK(Tests, Committed && *Committed);
			PF_CHECK(Tests, !History.IsActive() && History.GetCommandCount() == 1 && History.GetCursor() == 1);
			PF_CHECK(Tests, History.CanUndo() && History.GetUndoCommand() && History.GetUndoCommand()->Description == Description);
			PF_CHECK(Tests, CanRouteEditorShortcut({ .SceneAvailable = true }) && History.CanUndo());

			const auto Undone = History.Undo([&](std::string_view State) { return Restore(State, SceneValue, Selection); });
			PF_CHECK(Tests, Undone && *Undone);
			PF_CHECK(Tests, SceneValue.GetEntityCount() == 1 && Selection.GetPrimary() == OriginalID);
			const auto RestoredOriginal = SceneValue.FindEntity(OriginalID);
			PF_CHECK(Tests, RestoredOriginal.has_value());
			if (!RestoredOriginal) return;
			const auto RestoredOriginalTransform = RestoredOriginal->GetTransform();
			PF_CHECK(Tests, RestoredOriginalTransform &&
								glm::length(RestoredOriginalTransform->Translation - Initial.Translation) < 1.0e-5f &&
								glm::length(RestoredOriginalTransform->Scale - Initial.Scale) < 1.0e-5f &&
								1.0f - std::abs(glm::dot(RestoredOriginalTransform->Rotation, Initial.Rotation)) < 1.0e-5f);

			const auto Redone = History.Redo([&](std::string_view State) { return Restore(State, SceneValue, Selection); });
			PF_CHECK(Tests, Redone && *Redone);
			if (Duplicate)
			{
				PF_CHECK(Tests, DuplicateID.has_value() && SceneValue.GetEntityCount() == 2);
				if (!DuplicateID) return;
				const auto RedoneDuplicate = SceneValue.FindEntity(*DuplicateID);
				PF_CHECK(Tests, RedoneDuplicate.has_value() && Selection.GetPrimary() == *DuplicateID);
				if (!RedoneDuplicate) return;
				const auto RedoneTransform = RedoneDuplicate->GetTransform();
				PF_CHECK(Tests, RedoneTransform && glm::length(RedoneTransform->Translation - Final.Translation) < 1.0e-5f &&
									glm::length(RedoneTransform->Scale - Final.Scale) < 1.0e-5f &&
									1.0f - std::abs(glm::dot(RedoneTransform->Rotation, Final.Rotation)) < 1.0e-5f);
			}
			else
			{
				const auto RedoneOriginal = SceneValue.FindEntity(OriginalID);
				PF_CHECK(Tests, SceneValue.GetEntityCount() == 1 && RedoneOriginal.has_value());
				if (!RedoneOriginal) return;
				const auto RedoneTransform = RedoneOriginal->GetTransform();
				PF_CHECK(Tests, RedoneTransform && glm::length(RedoneTransform->Translation - Final.Translation) < 1.0e-5f &&
									glm::length(RedoneTransform->Scale - Final.Scale) < 1.0e-5f &&
									1.0f - std::abs(glm::dot(RedoneTransform->Rotation, Final.Rotation)) < 1.0e-5f);
			}
		};

		RunSingleGesture(TransformGizmoOperation::Translate, false, "Move Selection");
		RunSingleGesture(TransformGizmoOperation::Translate, true, "Move Selection");
		RunSingleGesture(TransformGizmoOperation::Rotate, false, "Rotate Selection");
		RunSingleGesture(TransformGizmoOperation::Rotate, true, "Rotate Selection");
		RunSingleGesture(TransformGizmoOperation::Scale, false, "Scale Selection");

		{
			Scene SceneValue;
			EditorSelection Selection;
			EditorCommandHistory History;
			History.Reset();
			auto Original = SceneValue.CreateEntity("Original");
			PF_CHECK(Tests, Original.has_value());
			if (!Original) return;
			const UUID OriginalID = Original->GetUUID();
			Selection.SelectSingle(OriginalID);
			auto BeforeDuplicate = Capture(SceneValue, Selection);
			PF_CHECK(Tests, BeforeDuplicate && History.Begin("Duplicate Entities", *BeforeDuplicate));
			if (!BeforeDuplicate || !History.IsActive()) return;
			auto Duplicate = SceneValue.DuplicateEntity(*Original);
			PF_CHECK(Tests, Duplicate.has_value());
			if (!Duplicate) return;
			const UUID DuplicateID = Duplicate->GetUUID();
			Selection.SelectSingle(DuplicateID);
			auto AfterDuplicate = Capture(SceneValue, Selection);
			PF_CHECK(Tests, AfterDuplicate && History.Commit(std::move(*AfterDuplicate)));
			if (!History.CanUndo()) return;

			auto DuplicateTransform = Duplicate->GetTransform();
			PF_CHECK(Tests, DuplicateTransform.has_value());
			if (!DuplicateTransform) return;
			const TransformComponent BeforeMoveTransform = *DuplicateTransform;
			auto BeforeMove = Capture(SceneValue, Selection);
			PF_CHECK(Tests, BeforeMove && History.Begin("Move Selection", *BeforeMove));
			if (!BeforeMove || !History.IsActive()) return;
			const bool InspectorPrematurelyFinishesMove = ShouldFinishSceneEditForInspector(History.IsActive(), NoInspectorHistoryItem, 0);
			PF_CHECK(Tests, !InspectorPrematurelyFinishesMove && History.IsActive() && !History.CanUndo());
			if (InspectorPrematurelyFinishesMove)
			{
				auto InspectorBoundary = Capture(SceneValue, Selection);
				if (InspectorBoundary) (void)History.Commit(std::move(*InspectorBoundary));
			}
			TransformComponent AfterMoveTransform = BeforeMoveTransform;
			AfterMoveTransform.Translation += glm::vec3(0.5f, 1.0f, -0.25f);
			PF_CHECK(Tests, Duplicate->SetTransform(AfterMoveTransform));
			PF_CHECK(Tests, GetViewportGizmoPointerOutcome({ true, false, true }, false) == ViewportGizmoPointerOutcome::Complete);
			auto AfterMove = Capture(SceneValue, Selection);
			PF_CHECK(Tests, AfterMove && History.Commit(std::move(*AfterMove)) && History.GetCommandCount() == 2 &&
								History.GetCursor() == 2 && History.CanUndo() && History.GetUndoCommand()->Description == "Move Selection");

			const auto UndoMove = History.Undo([&](std::string_view State) { return Restore(State, SceneValue, Selection); });
			PF_CHECK(Tests, UndoMove && *UndoMove && SceneValue.FindEntity(DuplicateID) && Selection.GetPrimary() == DuplicateID);
			auto MovedBackDuplicate = SceneValue.FindEntity(DuplicateID);
			PF_CHECK(Tests, MovedBackDuplicate && MovedBackDuplicate->GetTransform() &&
								glm::length(MovedBackDuplicate->GetTransform()->Translation - BeforeMoveTransform.Translation) < 1.0e-5f);
			const auto UndoDuplicate = History.Undo([&](std::string_view State) { return Restore(State, SceneValue, Selection); });
			PF_CHECK(Tests, UndoDuplicate && *UndoDuplicate && !SceneValue.FindEntity(DuplicateID) && Selection.GetPrimary() == OriginalID);
			const auto RedoDuplicate = History.Redo([&](std::string_view State) { return Restore(State, SceneValue, Selection); });
			PF_CHECK(Tests, RedoDuplicate && *RedoDuplicate && SceneValue.FindEntity(DuplicateID));
			const auto RedoMove = History.Redo([&](std::string_view State) { return Restore(State, SceneValue, Selection); });
			PF_CHECK(Tests, RedoMove && *RedoMove && Selection.GetPrimary() == DuplicateID);
			auto MovedDuplicate = SceneValue.FindEntity(DuplicateID);
			PF_CHECK(Tests, MovedDuplicate && MovedDuplicate->GetTransform() &&
								glm::length(MovedDuplicate->GetTransform()->Translation - AfterMoveTransform.Translation) < 1.0e-5f);
		}

		{
			Scene SceneValue;
			EditorSelection Selection;
			EditorCommandHistory History;
			History.Reset();
			auto Original = SceneValue.CreateEntity("Original");
			PF_CHECK(Tests, Original.has_value());
			if (!Original) return;
			const UUID OriginalID = Original->GetUUID();
			Selection.SelectSingle(OriginalID);
			auto Before = Capture(SceneValue, Selection);
			PF_CHECK(Tests, Before.has_value());
			if (!Before) return;
			const auto Begun = History.Begin("Duplicate Entities", *Before);
			PF_CHECK(Tests, Begun.has_value());
			if (!Begun) return;
			auto Duplicate = SceneValue.DuplicateEntity(*Original);
			PF_CHECK(Tests, Duplicate.has_value());
			if (!Duplicate) return;
			const UUID DuplicateID = Duplicate->GetUUID();
			Selection.SelectSingle(DuplicateID);
			auto After = Capture(SceneValue, Selection);
			PF_CHECK(Tests, After.has_value());
			if (!After) return;
			const auto Committed = History.Commit(std::move(*After));
			PF_CHECK(Tests, Committed && *Committed && History.CanUndo() && History.GetUndoCommand()->Description == "Duplicate Entities");
			const auto Undo = History.Undo([&](std::string_view State) { return Restore(State, SceneValue, Selection); });
			PF_CHECK(Tests, Undo && *Undo && SceneValue.GetEntityCount() == 1 && Selection.GetPrimary() == OriginalID &&
								!SceneValue.FindEntity(DuplicateID));
			const auto Redo = History.Redo([&](std::string_view State) { return Restore(State, SceneValue, Selection); });
			PF_CHECK(Tests, Redo && *Redo && SceneValue.GetEntityCount() == 2 && Selection.GetPrimary() == DuplicateID &&
								SceneValue.FindEntity(DuplicateID));
		}

		{
			Scene SceneValue;
			EditorSelection Selection;
			EditorCommandHistory History;
			History.Reset();
			auto OriginalParent = SceneValue.CreateEntity("Parent");
			auto OriginalChild = SceneValue.CreateEntity("Child");
			auto SecondRoot = SceneValue.CreateEntity("Second root");
			PF_CHECK(Tests, OriginalParent && OriginalChild && SecondRoot);
			if (!OriginalParent || !OriginalChild || !SecondRoot) return;
			PF_CHECK(Tests, OriginalChild->SetParent(*OriginalParent));
			PointLightComponent ChildLight;
			ChildLight.Intensity = 4.0f;
			PF_CHECK(Tests, OriginalChild->SetPointLight(ChildLight));
			const UUID ParentID = OriginalParent->GetUUID();
			const UUID ChildID = OriginalChild->GetUUID();
			const UUID SecondID = SecondRoot->GetUUID();
			Selection.Set({ ParentID, SecondID }, ParentID);
			auto Before = Capture(SceneValue, Selection);
			PF_CHECK(Tests, Before.has_value());
			if (!Before) return;
			const auto Begun = History.Begin("Move Selection", *Before);
			PF_CHECK(Tests, Begun.has_value());
			if (!Begun) return;
			PF_CHECK(Tests, !ShouldFinishSceneEditForInspector(History.IsActive(), NoInspectorHistoryItem, 0) && History.IsActive());

			auto DuplicateParent = SceneValue.DuplicateEntity(*OriginalParent);
			auto DuplicateChild = SceneValue.DuplicateEntity(*OriginalChild);
			auto DuplicateSecond = SceneValue.DuplicateEntity(*SecondRoot);
			PF_CHECK(Tests, DuplicateParent && DuplicateChild && DuplicateSecond);
			if (!DuplicateParent || !DuplicateChild || !DuplicateSecond) return;
			PF_CHECK(Tests, DuplicateChild->SetParent(*DuplicateParent));
			const UUID DuplicateParentID = DuplicateParent->GetUUID();
			const UUID DuplicateChildID = DuplicateChild->GetUUID();
			const UUID DuplicateSecondID = DuplicateSecond->GetUUID();
			for (Entity* Root : std::array<Entity*, 2>{ &*DuplicateParent, &*DuplicateSecond })
			{
				const auto Transform = Root->GetTransform();
				PF_CHECK(Tests, Transform.has_value());
				if (Transform)
				{
					auto Moved = *Transform;
					Moved.Translation.x += 2.0f;
					PF_CHECK(Tests, Root->SetTransform(Moved));
				}
			}
			Selection.Set({ DuplicateParentID, DuplicateSecondID }, DuplicateParentID);
			PF_CHECK(Tests, !ShouldFinishSceneEditForInspector(History.IsActive(), NoInspectorHistoryItem, 0) && History.IsActive() &&
								!History.CanUndo());
			PF_CHECK(Tests, GetViewportGizmoPointerOutcome({ true, false, true }, false) == ViewportGizmoPointerOutcome::Complete);
			auto After = Capture(SceneValue, Selection);
			PF_CHECK(Tests, After.has_value());
			if (!After) return;
			const auto Committed = History.Commit(std::move(*After));
			PF_CHECK(Tests, Committed && *Committed);
			PF_CHECK(Tests, History.GetCommandCount() == 1 && History.CanUndo() && SceneValue.GetEntityCount() == 6);
			const auto Undo = History.Undo([&](std::string_view State) { return Restore(State, SceneValue, Selection); });
			PF_CHECK(Tests,
				Undo && *Undo && SceneValue.GetEntityCount() == 3 && Selection.GetPrimary() == ParentID && Selection.Contains(SecondID));
			const auto Redo = History.Redo([&](std::string_view State) { return Restore(State, SceneValue, Selection); });
			PF_CHECK(Tests, Redo && *Redo && SceneValue.GetEntityCount() == 6 && Selection.GetPrimary() == DuplicateParentID &&
								Selection.Contains(DuplicateSecondID));
			const auto RestoredDuplicateChild = SceneValue.FindEntity(DuplicateChildID);
			PF_CHECK(Tests, RestoredDuplicateChild.has_value());
			if (RestoredDuplicateChild)
			{
				const auto Parent = RestoredDuplicateChild->GetParent();
				const auto Light = RestoredDuplicateChild->GetPointLight();
				PF_CHECK(Tests, Parent && Parent->has_value() && (**Parent).GetUUID() == DuplicateParentID);
				PF_CHECK(Tests, Light && Light->has_value() && Light->value().Intensity == 4.0f);
			}
			PF_CHECK(Tests, SceneValue.FindEntity(ParentID) && SceneValue.FindEntity(ChildID) && SceneValue.FindEntity(SecondID) &&
								SceneValue.FindEntity(DuplicateSecondID));
		}

		{
			Scene SceneValue;
			EditorSelection Selection;
			EditorCommandHistory History;
			History.Reset();
			auto Original = SceneValue.CreateEntity("Original");
			PF_CHECK(Tests, Original.has_value());
			if (!Original) return;
			const UUID OriginalID = Original->GetUUID();
			Selection.SelectSingle(OriginalID);
			auto Before = Capture(SceneValue, Selection);
			PF_CHECK(Tests, Before.has_value());
			if (!Before) return;
			const auto Begun = History.Begin("Move Selection", *Before);
			PF_CHECK(Tests, Begun.has_value());
			if (!Begun) return;
			auto AbandonedCopy = SceneValue.DuplicateEntity(*Original);
			PF_CHECK(Tests, AbandonedCopy.has_value());
			if (!AbandonedCopy) return;
			const UUID AbandonedID = AbandonedCopy->GetUUID();
			Selection.SelectSingle(AbandonedID);
			PF_CHECK(Tests, GetViewportGizmoPointerOutcome({ true, true, true }, true) == ViewportGizmoPointerOutcome::Cancel);
			const auto Cancelled = History.Cancel([&](std::string_view State) { return Restore(State, SceneValue, Selection); });
			PF_CHECK(Tests, Cancelled && !History.IsActive() && History.GetCommandCount() == 0 && !History.CanUndo() &&
								SceneValue.GetEntityCount() == 1 && Selection.GetPrimary() == OriginalID &&
								!SceneValue.FindEntity(AbandonedID));
		}
	}

	void TestSceneCommandHistoryRestoresIdentity(TestRunner& Tests)
	{
		using namespace PulseForge;
		Scene SceneValue;
		const auto Root = SceneValue.CreateEntity("Parent");
		PF_CHECK(Tests, Root.has_value());
		if (!Root) return;

		PulseForgeEditor::EditorCommandHistory History;
		History.Reset();
		const auto Before = SceneSerializer::Serialize(SceneValue);
		PF_CHECK(Tests, Before.has_value());
		if (!Before) return;
		PF_CHECK(Tests, History.Begin("Create child", *Before));

		auto Child = SceneValue.CreateEntity("Shadow Caster");
		PF_CHECK(Tests, Child.has_value());
		if (!Child) return;
		const UUID ChildIdentifier = Child->GetUUID();
		TransformComponent Transform;
		Transform.Translation = { 1.0f, 2.0f, 3.0f };
		PF_CHECK(Tests, Child->SetTransform(Transform));
		PointLightComponent Light;
		Light.CastShadows = true;
		PF_CHECK(Tests, Child->SetPointLight(Light));
		PF_CHECK(Tests, Child->SetParent(*Root));
		const auto After = SceneSerializer::Serialize(SceneValue);
		PF_CHECK(Tests, After.has_value());
		if (!After) return;
		PF_CHECK(Tests, History.Commit(*After));

		auto Restore = [&SceneValue](std::string_view State) -> std::expected<void, std::string>
		{
			if (auto Result = SceneSerializer::Deserialize(State, SceneValue); !Result) return std::unexpected(Result.error().Message);
			return {};
		};
		const auto UndoCreate = History.Undo(Restore);
		PF_CHECK(Tests, UndoCreate && *UndoCreate && !SceneValue.FindEntity(ChildIdentifier));
		PF_CHECK(Tests, SceneValue.FindEntity(Root->GetUUID()).has_value());
		const auto RedoCreate = History.Redo(Restore);
		PF_CHECK(Tests, RedoCreate && *RedoCreate);
		const auto RestoredChild = SceneValue.FindEntity(ChildIdentifier);
		PF_CHECK(Tests, RestoredChild.has_value());
		if (RestoredChild)
		{
			const auto RestoredTransform = RestoredChild->GetTransform();
			const auto RestoredLight = RestoredChild->GetPointLight();
			const auto RestoredParent = RestoredChild->GetParent();
			PF_CHECK(Tests, RestoredTransform && RestoredTransform->Translation == glm::vec3(1.0f, 2.0f, 3.0f));
			PF_CHECK(Tests, RestoredLight && RestoredLight->has_value() && RestoredLight->value().CastShadows);
			PF_CHECK(Tests, RestoredParent && RestoredParent->has_value() && (**RestoredParent).GetUUID() == Root->GetUUID());
		}

		History.MarkSaved();
		const auto DeleteBefore = SceneSerializer::Serialize(SceneValue);
		PF_CHECK(Tests, DeleteBefore && History.Begin("Delete child", *DeleteBefore));
		const auto CurrentChild = SceneValue.FindEntity(ChildIdentifier);
		PF_CHECK(Tests, CurrentChild && SceneValue.DestroyEntity(*CurrentChild));
		const auto DeleteAfter = SceneSerializer::Serialize(SceneValue);
		PF_CHECK(Tests, DeleteAfter && History.Commit(*DeleteAfter));
		PF_CHECK(Tests, History.IsDirty() && !SceneValue.FindEntity(ChildIdentifier));
		const auto UndoDelete = History.Undo(Restore);
		PF_CHECK(Tests, UndoDelete && *UndoDelete && SceneValue.FindEntity(ChildIdentifier));
		PF_CHECK(Tests, !History.IsDirty());
	}

} // namespace

void RunEditorHistoryTests(TestRunner& Tests)
{
	TestEditorCommandHistory(Tests);
	TestAltGizmoDragHistoryIntegration(Tests);
	TestSceneCommandHistoryRestoresIdentity(Tests);
}
