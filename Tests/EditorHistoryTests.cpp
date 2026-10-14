#include "TestEditorSupport.h"
#include "Editor/EditorSceneEditController.h"

namespace
{
	void TestConsoleFormatting(TestRunner& Tests)
	{
		using PulseForgeEditor::ConsoleEntryView;
		using PulseForgeEditor::FormatConsoleEntry;
		using PulseForgeEditor::FormatConsoleHistory;

		PF_CHECK(Tests, FormatConsoleEntry({ "info", "PULSEFORGE", "Renderer ready" }) ==
			"[info] [PULSEFORGE] Renderer ready");
		PF_CHECK(Tests, FormatConsoleEntry({ "warning", "APP", "Slow asset" }) ==
			"[warning] [APP] Slow asset");
		PF_CHECK(Tests, FormatConsoleEntry({ "error", "PULSEFORGE", "Failed" }) ==
			"[error] [PULSEFORGE] Failed");
		PF_CHECK(Tests, FormatConsoleEntry({ "critical", "APP", "Fatal" }) ==
			"[critical] [APP] Fatal");
		PF_CHECK(Tests, FormatConsoleEntry({ "trace", "APP", "Trace" }) == "[trace] [APP] Trace");
		PF_CHECK(Tests, FormatConsoleEntry({ "debug", "APP", "Debug" }) == "[debug] [APP] Debug");
		PF_CHECK(Tests, FormatConsoleEntry({ {}, "APP", "Unknown severity" }) == "[unknown] [APP] Unknown severity");
		PF_CHECK(Tests, FormatConsoleEntry({ "info", {}, "No logger" }) == "[info] No logger");
		PF_CHECK(Tests, FormatConsoleEntry({ "info", "APP", {} }) == "[info] [APP]");
		PF_CHECK(Tests, FormatConsoleEntry({ "info", {}, {} }) == "[info]");
		PF_CHECK(Tests, FormatConsoleEntry({ "info", "APP", "UTF-8: \xE2\x98\x83 and punctuation !?[]" }) ==
			"[info] [APP] UTF-8: \xE2\x98\x83 and punctuation !?[]");
		PF_CHECK(Tests, FormatConsoleEntry({ "debug", "APP", "tabs\tremain" }) == "[debug] [APP] tabs\tremain");

		const std::string TruncatedPayload = std::string(8192, 'x') + " [truncated]";
		PF_CHECK(Tests, FormatConsoleEntry({ "warning", "APP", TruncatedPayload }) ==
			"[warning] [APP] " + TruncatedPayload);

		PF_CHECK(Tests, FormatConsoleHistory({}).empty());
		const std::array Single = { ConsoleEntryView{ "info", "PULSEFORGE", "Only entry" } };
		PF_CHECK(Tests, FormatConsoleHistory(Single) == "[info] [PULSEFORGE] Only entry");
		const std::array Ordered = {
			ConsoleEntryView{ "info", "PULSEFORGE", "First" },
			ConsoleEntryView{ "warning", "APP", "Second" },
			ConsoleEntryView{ "error", "PULSEFORGE", "Third" }
		};
		const std::string FormattedHistory = FormatConsoleHistory(Ordered);
		PF_CHECK(Tests, FormattedHistory ==
			"[info] [PULSEFORGE] First\n[warning] [APP] Second\n[error] [PULSEFORGE] Third");
		PF_CHECK(Tests, !FormattedHistory.empty() && FormattedHistory.back() != '\n');
		PF_CHECK(Tests, std::count(FormattedHistory.begin(), FormattedHistory.end(), '\n') == 2);

		std::vector<ConsoleEntryView> CapacityBoundary;
		CapacityBoundary.reserve(2000);
		for (size_t Index = 0; Index < 2000; ++Index)
			CapacityBoundary.push_back({ "info", "APP", "retained" });
		const std::string CapacityHistory = FormatConsoleHistory(CapacityBoundary);
		PF_CHECK(Tests, std::count(CapacityHistory.begin(), CapacityHistory.end(), '\n') == 1999);
		PF_CHECK(Tests, CapacityHistory.starts_with("[info] [APP] retained\n"));
		PF_CHECK(Tests, CapacityHistory.ends_with("[info] [APP] retained"));
	}

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
		const auto Cancelled = ByteLimited.Cancel([&Current](std::string_view State) -> std::expected<void, std::string>
		{
			Current.assign(State);
			return {};
		});
		PF_CHECK(Tests, Cancelled && Current == "1234" && !ByteLimited.IsActive());

		PF_CHECK(Tests, ByteLimited.Begin("Failed restore", "a"));
		Current = "b";
		PF_CHECK(Tests, ByteLimited.Commit(Current));
		const auto FailedUndo = ByteLimited.Undo([](std::string_view) -> std::expected<void, std::string>
		{
			return std::unexpected("restore rejected");
		});
		PF_CHECK(Tests, !FailedUndo && ByteLimited.CanUndo() && Current == "b");
	}

	void TestEditorInteractionPolicy(TestRunner& Tests)
	{
		using namespace PulseForgeEditor;
		const EditorShortcutContext OrdinarySceneFocus{ .SceneAvailable = true };
		PF_CHECK(Tests, CanRouteEditorShortcut(OrdinarySceneFocus));
		PF_CHECK(Tests, CanRouteEditorShortcut({ .SceneAvailable = true, .GizmoInteractionActive = false }));
		PF_CHECK(Tests, !CanRouteEditorShortcut({ .SceneAvailable = true, .GizmoInteractionActive = true }));
		PF_CHECK(Tests, !CanRouteEditorShortcut({ .SceneAvailable = true, .TextInputActive = true }));
		PF_CHECK(Tests, !CanRouteEditorShortcut({ .SceneAvailable = true, .ImGuiItemActive = true }));
		PF_CHECK(Tests, !CanRouteEditorShortcut({ .SceneAvailable = true, .PopupOpen = true }));
		PF_CHECK(Tests, !CanRouteEditorShortcut({ .SceneAvailable = true, .MenuNavigationActive = true }));
		PF_CHECK(Tests, !CanRouteEditorShortcut({ .SceneAvailable = true, .WindowNavigationActive = true }));
		PF_CHECK(Tests, !CanRouteEditorShortcut({ .SceneAvailable = true, .RuntimeActive = true }));
		PF_CHECK(Tests, !CanRouteEditorShortcut({ .SceneAvailable = true, .CameraNavigationActive = true }));

		PF_CHECK(Tests, GetViewportGizmoPointerOutcome({ true, true, false }, false) ==
			ViewportGizmoPointerOutcome::Continue);
		PF_CHECK(Tests, GetViewportGizmoPointerOutcome({ true, false, true }, false) ==
			ViewportGizmoPointerOutcome::Complete);
		PF_CHECK(Tests, GetViewportGizmoPointerOutcome({ true, false, false }, false) ==
			ViewportGizmoPointerOutcome::Complete);
		PF_CHECK(Tests, GetViewportGizmoPointerOutcome({ true, true, true }, true) ==
			ViewportGizmoPointerOutcome::Cancel);
		PF_CHECK(Tests, GetViewportGizmoPointerOutcome({ false, false, true }, false) ==
			ViewportGizmoPointerOutcome::Idle);
		PF_CHECK(Tests, ShouldFinishSceneEditForInspector(true, 71, 0));
		PF_CHECK(Tests, !ShouldFinishSceneEditForInspector(true, 71, 71));
		PF_CHECK(Tests, !ShouldFinishSceneEditForInspector(true, NoInspectorHistoryItem, 0));
		PF_CHECK(Tests, !ShouldFinishSceneEditForInspector(false, 71, 0));
	}

	void TestAltGizmoDragHistoryIntegration(TestRunner& Tests)
	{
		using namespace PulseForge;
		using namespace PulseForgeEditor;

		const auto Capture = [](const Scene& SceneValue, const EditorSelection& Selection)
			-> std::expected<std::string, std::string>
		{
			auto SerializedScene = SceneSerializer::Serialize(SceneValue);
			if (!SerializedScene)
				return std::unexpected(SerializedScene.error().Message);
			try
			{
				nlohmann::json State;
				State["scene"] = nlohmann::json::parse(*SerializedScene);
				State["selection"] = nlohmann::json::array();
				for (const UUID& Identifier : Selection.GetEntities())
					State["selection"].push_back(Identifier.ToString());
				State["primary"] = Selection.GetPrimary()
					? Selection.GetPrimary()->ToString()
					: std::string{};
				return State.dump();
			}
			catch (const std::exception& Error)
			{
				return std::unexpected(Error.what());
			}
		};
		const auto Restore = [](std::string_view StateData, Scene& SceneValue, EditorSelection& Selection)
			-> std::expected<void, std::string>
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
					if (!Identifier)
						return std::unexpected(Identifier.error().Message);
					if (SceneValue.FindEntity(*Identifier))
						Identifiers.push_back(*Identifier);
				}
				std::optional<UUID> Primary;
				const std::string SerializedPrimary = State.at("primary").get<std::string>();
				if (!SerializedPrimary.empty())
				{
					auto Identifier = UUID::Parse(SerializedPrimary);
					if (!Identifier)
						return std::unexpected(Identifier.error().Message);
					if (SceneValue.FindEntity(*Identifier))
						Primary = *Identifier;
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
			if (!Original)
				return;
			TransformComponent Initial;
			Initial.Translation = { 1.0f, 2.0f, 3.0f };
			Initial.Scale = { 1.5f, 0.8f, 1.2f };
			PF_CHECK(Tests, Original->SetTransform(Initial));
			const UUID OriginalID = Original->GetUUID();
			Selection.SelectSingle(OriginalID);
			auto Before = Capture(SceneValue, Selection);
			PF_CHECK(Tests, Before.has_value());
			if (!Before || !History.Begin(Description, *Before))
				return;

			// The viewport is drawn before the Inspector. The production Inspector policy must leave
			// a gizmo-owned transaction active while the gesture is still changing scene state.
			const uint32_t GizmoInspectorItem = NoInspectorHistoryItem;
			const bool PrematurelyFinished = ShouldFinishSceneEditForInspector(
				History.IsActive(), GizmoInspectorItem, 0);
			PF_CHECK(Tests, !PrematurelyFinished && History.IsActive() && !History.CanUndo());
			if (PrematurelyFinished)
			{
				auto InspectorBoundary = Capture(SceneValue, Selection);
				if (InspectorBoundary)
					(void)History.Commit(std::move(*InspectorBoundary));
			}

			PF_CHECK(Tests, GetViewportGizmoPointerOutcome({ true, true, false }, false) ==
				ViewportGizmoPointerOutcome::Continue);
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
			else
				Target = *Original;

			TransformComponent Final = Initial;
			if (Operation == TransformGizmoOperation::Translate)
				Final.Translation += glm::vec3(2.5f, -0.25f, 1.0f);
			else if (Operation == TransformGizmoOperation::Rotate)
				Final.Rotation = glm::normalize(glm::angleAxis(glm::radians(37.0f), glm::vec3(0.0f, 1.0f, 0.0f)) *
					Initial.Rotation);
			else
				Final.Scale *= glm::vec3(1.25f, 0.75f, 1.5f);
			PF_CHECK(Tests, Target->SetTransform(Final));
			PF_CHECK(Tests, !ShouldFinishSceneEditForInspector(
				History.IsActive(), GizmoInspectorItem, 0));
			PF_CHECK(Tests, History.IsActive() && !History.CanUndo());

			const auto Release = GetViewportGizmoPointerOutcome({ true, false, true }, false);
			PF_CHECK(Tests, Release == ViewportGizmoPointerOutcome::Complete);
			if (Release != ViewportGizmoPointerOutcome::Complete)
				return;
			auto After = Capture(SceneValue, Selection);
			PF_CHECK(Tests, After.has_value());
			if (!After)
				return;
			const auto Committed = History.Commit(std::move(*After));
			PF_CHECK(Tests, Committed && *Committed);
			PF_CHECK(Tests, !History.IsActive() && History.GetCommandCount() == 1 && History.GetCursor() == 1);
			PF_CHECK(Tests, History.CanUndo() && History.GetUndoCommand() &&
				History.GetUndoCommand()->Description == Description);
			PF_CHECK(Tests, CanRouteEditorShortcut({ .SceneAvailable = true }) && History.CanUndo());

			const auto Undone = History.Undo([&](std::string_view State) { return Restore(State, SceneValue, Selection); });
			PF_CHECK(Tests, Undone && *Undone);
			PF_CHECK(Tests, SceneValue.GetEntityCount() == 1 && Selection.GetPrimary() == OriginalID);
			const auto RestoredOriginal = SceneValue.FindEntity(OriginalID);
			PF_CHECK(Tests, RestoredOriginal.has_value());
			if (!RestoredOriginal)
				return;
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
				if (!DuplicateID)
					return;
				const auto RedoneDuplicate = SceneValue.FindEntity(*DuplicateID);
				PF_CHECK(Tests, RedoneDuplicate.has_value() && Selection.GetPrimary() == *DuplicateID);
				if (!RedoneDuplicate)
					return;
				const auto RedoneTransform = RedoneDuplicate->GetTransform();
				PF_CHECK(Tests, RedoneTransform &&
					glm::length(RedoneTransform->Translation - Final.Translation) < 1.0e-5f &&
					glm::length(RedoneTransform->Scale - Final.Scale) < 1.0e-5f &&
					1.0f - std::abs(glm::dot(RedoneTransform->Rotation, Final.Rotation)) < 1.0e-5f);
			}
			else
			{
				const auto RedoneOriginal = SceneValue.FindEntity(OriginalID);
				PF_CHECK(Tests, SceneValue.GetEntityCount() == 1 && RedoneOriginal.has_value());
				if (!RedoneOriginal)
					return;
				const auto RedoneTransform = RedoneOriginal->GetTransform();
				PF_CHECK(Tests, RedoneTransform &&
					glm::length(RedoneTransform->Translation - Final.Translation) < 1.0e-5f &&
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
			if (!Original)
				return;
			const UUID OriginalID = Original->GetUUID();
			Selection.SelectSingle(OriginalID);
			auto BeforeDuplicate = Capture(SceneValue, Selection);
			PF_CHECK(Tests, BeforeDuplicate && History.Begin("Duplicate Entities", *BeforeDuplicate));
			if (!BeforeDuplicate || !History.IsActive())
				return;
			auto Duplicate = SceneValue.DuplicateEntity(*Original);
			PF_CHECK(Tests, Duplicate.has_value());
			if (!Duplicate)
				return;
			const UUID DuplicateID = Duplicate->GetUUID();
			Selection.SelectSingle(DuplicateID);
			auto AfterDuplicate = Capture(SceneValue, Selection);
			PF_CHECK(Tests, AfterDuplicate && History.Commit(std::move(*AfterDuplicate)));
			if (!History.CanUndo())
				return;

			auto DuplicateTransform = Duplicate->GetTransform();
			PF_CHECK(Tests, DuplicateTransform.has_value());
			if (!DuplicateTransform)
				return;
			const TransformComponent BeforeMoveTransform = *DuplicateTransform;
			auto BeforeMove = Capture(SceneValue, Selection);
			PF_CHECK(Tests, BeforeMove && History.Begin("Move Selection", *BeforeMove));
			if (!BeforeMove || !History.IsActive())
				return;
			const bool InspectorPrematurelyFinishesMove = ShouldFinishSceneEditForInspector(
				History.IsActive(), NoInspectorHistoryItem, 0);
			PF_CHECK(Tests, !InspectorPrematurelyFinishesMove && History.IsActive() && !History.CanUndo());
			if (InspectorPrematurelyFinishesMove)
			{
				auto InspectorBoundary = Capture(SceneValue, Selection);
				if (InspectorBoundary)
					(void)History.Commit(std::move(*InspectorBoundary));
			}
			TransformComponent AfterMoveTransform = BeforeMoveTransform;
			AfterMoveTransform.Translation += glm::vec3(0.5f, 1.0f, -0.25f);
			PF_CHECK(Tests, Duplicate->SetTransform(AfterMoveTransform));
			PF_CHECK(Tests, GetViewportGizmoPointerOutcome({ true, false, true }, false) ==
				ViewportGizmoPointerOutcome::Complete);
			auto AfterMove = Capture(SceneValue, Selection);
			PF_CHECK(Tests, AfterMove && History.Commit(std::move(*AfterMove)) && History.GetCommandCount() == 2 &&
				History.GetCursor() == 2 && History.CanUndo() &&
				History.GetUndoCommand()->Description == "Move Selection");

			const auto UndoMove = History.Undo([&](std::string_view State)
				{ return Restore(State, SceneValue, Selection); });
			PF_CHECK(Tests, UndoMove && *UndoMove && SceneValue.FindEntity(DuplicateID) &&
				Selection.GetPrimary() == DuplicateID);
			auto MovedBackDuplicate = SceneValue.FindEntity(DuplicateID);
			PF_CHECK(Tests, MovedBackDuplicate && MovedBackDuplicate->GetTransform() &&
				glm::length(MovedBackDuplicate->GetTransform()->Translation - BeforeMoveTransform.Translation) < 1.0e-5f);
			const auto UndoDuplicate = History.Undo([&](std::string_view State)
				{ return Restore(State, SceneValue, Selection); });
			PF_CHECK(Tests, UndoDuplicate && *UndoDuplicate && !SceneValue.FindEntity(DuplicateID) &&
				Selection.GetPrimary() == OriginalID);
			const auto RedoDuplicate = History.Redo([&](std::string_view State)
				{ return Restore(State, SceneValue, Selection); });
			PF_CHECK(Tests, RedoDuplicate && *RedoDuplicate && SceneValue.FindEntity(DuplicateID));
			const auto RedoMove = History.Redo([&](std::string_view State)
				{ return Restore(State, SceneValue, Selection); });
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
			if (!Original)
				return;
			const UUID OriginalID = Original->GetUUID();
			Selection.SelectSingle(OriginalID);
			auto Before = Capture(SceneValue, Selection);
			PF_CHECK(Tests, Before.has_value());
			if (!Before)
				return;
			const auto Begun = History.Begin("Duplicate Entities", *Before);
			PF_CHECK(Tests, Begun.has_value());
			if (!Begun)
				return;
			auto Duplicate = SceneValue.DuplicateEntity(*Original);
			PF_CHECK(Tests, Duplicate.has_value());
			if (!Duplicate)
				return;
			const UUID DuplicateID = Duplicate->GetUUID();
			Selection.SelectSingle(DuplicateID);
			auto After = Capture(SceneValue, Selection);
			PF_CHECK(Tests, After.has_value());
			if (!After)
				return;
			const auto Committed = History.Commit(std::move(*After));
			PF_CHECK(Tests, Committed && *Committed && History.CanUndo() &&
				History.GetUndoCommand()->Description == "Duplicate Entities");
			const auto Undo = History.Undo([&](std::string_view State) { return Restore(State, SceneValue, Selection); });
			PF_CHECK(Tests, Undo && *Undo && SceneValue.GetEntityCount() == 1 &&
				Selection.GetPrimary() == OriginalID && !SceneValue.FindEntity(DuplicateID));
			const auto Redo = History.Redo([&](std::string_view State) { return Restore(State, SceneValue, Selection); });
			PF_CHECK(Tests, Redo && *Redo && SceneValue.GetEntityCount() == 2 &&
				Selection.GetPrimary() == DuplicateID && SceneValue.FindEntity(DuplicateID));
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
			if (!OriginalParent || !OriginalChild || !SecondRoot)
				return;
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
			if (!Before)
				return;
			const auto Begun = History.Begin("Move Selection", *Before);
			PF_CHECK(Tests, Begun.has_value());
			if (!Begun)
				return;
			PF_CHECK(Tests, !ShouldFinishSceneEditForInspector(
				History.IsActive(), NoInspectorHistoryItem, 0) && History.IsActive());

			auto DuplicateParent = SceneValue.DuplicateEntity(*OriginalParent);
			auto DuplicateChild = SceneValue.DuplicateEntity(*OriginalChild);
			auto DuplicateSecond = SceneValue.DuplicateEntity(*SecondRoot);
			PF_CHECK(Tests, DuplicateParent && DuplicateChild && DuplicateSecond);
			if (!DuplicateParent || !DuplicateChild || !DuplicateSecond)
				return;
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
			PF_CHECK(Tests, !ShouldFinishSceneEditForInspector(
				History.IsActive(), NoInspectorHistoryItem, 0) && History.IsActive() && !History.CanUndo());
			PF_CHECK(Tests, GetViewportGizmoPointerOutcome({ true, false, true }, false) ==
				ViewportGizmoPointerOutcome::Complete);
			auto After = Capture(SceneValue, Selection);
			PF_CHECK(Tests, After.has_value());
			if (!After)
				return;
			const auto Committed = History.Commit(std::move(*After));
			PF_CHECK(Tests, Committed && *Committed);
			PF_CHECK(Tests, History.GetCommandCount() == 1 && History.CanUndo() && SceneValue.GetEntityCount() == 6);
			const auto Undo = History.Undo([&](std::string_view State) { return Restore(State, SceneValue, Selection); });
			PF_CHECK(Tests, Undo && *Undo && SceneValue.GetEntityCount() == 3 &&
				Selection.GetPrimary() == ParentID && Selection.Contains(SecondID));
			const auto Redo = History.Redo([&](std::string_view State) { return Restore(State, SceneValue, Selection); });
			PF_CHECK(Tests, Redo && *Redo && SceneValue.GetEntityCount() == 6 &&
				Selection.GetPrimary() == DuplicateParentID && Selection.Contains(DuplicateSecondID));
			const auto RestoredDuplicateChild = SceneValue.FindEntity(DuplicateChildID);
			PF_CHECK(Tests, RestoredDuplicateChild.has_value());
			if (RestoredDuplicateChild)
			{
				const auto Parent = RestoredDuplicateChild->GetParent();
				const auto Light = RestoredDuplicateChild->GetPointLight();
				PF_CHECK(Tests, Parent && Parent->has_value() && (**Parent).GetUUID() == DuplicateParentID);
				PF_CHECK(Tests, Light && Light->has_value() && Light->value().Intensity == 4.0f);
			}
			PF_CHECK(Tests, SceneValue.FindEntity(ParentID) && SceneValue.FindEntity(ChildID) &&
				SceneValue.FindEntity(SecondID) && SceneValue.FindEntity(DuplicateSecondID));
		}

		{
			Scene SceneValue;
			EditorSelection Selection;
			EditorCommandHistory History;
			History.Reset();
			auto Original = SceneValue.CreateEntity("Original");
			PF_CHECK(Tests, Original.has_value());
			if (!Original)
				return;
			const UUID OriginalID = Original->GetUUID();
			Selection.SelectSingle(OriginalID);
			auto Before = Capture(SceneValue, Selection);
			PF_CHECK(Tests, Before.has_value());
			if (!Before)
				return;
			const auto Begun = History.Begin("Move Selection", *Before);
			PF_CHECK(Tests, Begun.has_value());
			if (!Begun)
				return;
			auto AbandonedCopy = SceneValue.DuplicateEntity(*Original);
			PF_CHECK(Tests, AbandonedCopy.has_value());
			if (!AbandonedCopy)
				return;
			const UUID AbandonedID = AbandonedCopy->GetUUID();
			Selection.SelectSingle(AbandonedID);
			PF_CHECK(Tests, GetViewportGizmoPointerOutcome({ true, true, true }, true) ==
				ViewportGizmoPointerOutcome::Cancel);
			const auto Cancelled = History.Cancel([&](std::string_view State)
				{ return Restore(State, SceneValue, Selection); });
			PF_CHECK(Tests, Cancelled && !History.IsActive() && History.GetCommandCount() == 0 &&
				!History.CanUndo() && SceneValue.GetEntityCount() == 1 && Selection.GetPrimary() == OriginalID &&
				!SceneValue.FindEntity(AbandonedID));
		}
	}

	void TestEditorSelection(TestRunner& Tests)
	{
		using PulseForgeEditor::EditorSelection;
		using PulseForge::UUID;
		const UUID First{ 0, 201 };
		const UUID Second{ 0, 202 };
		const UUID Third{ 0, 203 };
		const UUID Fourth{ 0, 204 };
		const std::array<UUID, 4> VisibleOrder{ First, Second, Third, Fourth };

		EditorSelection Selection;
		Selection.SelectSingle(Second);
		PF_CHECK(Tests, Selection.GetEntities().size() == 1 && Selection.GetPrimary() == Second);
		Selection.Toggle(Third);
		PF_CHECK(Tests, Selection.GetEntities().size() == 2 && Selection.GetPrimary() == Third);
		PF_CHECK(Tests, Selection.SelectRange(VisibleOrder, Fourth));
		PF_CHECK(Tests, Selection.GetEntities().size() == 3 && Selection.Contains(Second) &&
			Selection.Contains(Third) && Selection.Contains(Fourth) && Selection.GetPrimary() == Fourth);
		PF_CHECK(Tests, Selection.SelectRange(VisibleOrder, First, true));
		PF_CHECK(Tests, Selection.GetEntities().size() == 4 && Selection.GetPrimary() == First);
		Selection.Remove(First);
		PF_CHECK(Tests, Selection.GetEntities().size() == 3 && Selection.GetPrimary() == Fourth);
		Selection.Toggle(Fourth);
		PF_CHECK(Tests, !Selection.Contains(Fourth) && Selection.GetPrimary() == Third);
		Selection.Set({ First, First, Third }, First);
		PF_CHECK(Tests, Selection.GetEntities().size() == 2 && Selection.GetPrimary() == First);
		Selection.Clear();
		PF_CHECK(Tests, Selection.GetEntities().empty() && !Selection.GetPrimary() && !Selection.GetAnchor());
	}

	void TestEditorVisibility(TestRunner& Tests)
	{
		using PulseForgeEditor::EditorVisibility;
		using PulseForge::UUID;
		const UUID Parent{ 0, 301 };
		const UUID Child{ 0, 302 };
		const UUID Sibling{ 0, 303 };
		const std::unordered_map<UUID, UUID, PulseForge::UUIDHash> Parents{ { Child, Parent } };
		const auto ParentLookup = [&Parents](const UUID& Identifier) -> std::optional<UUID>
		{
			const auto Found = Parents.find(Identifier);
			return Found == Parents.end() ? std::nullopt : std::optional<UUID>{ Found->second };
		};

		EditorVisibility Visibility;
		Visibility.SetVisible(Child, false);
		Visibility.SetVisible(Parent, false);
		PF_CHECK(Tests, !Visibility.IsEffectivelyVisible(Child, ParentLookup));
		Visibility.SetVisible(Parent, true);
		PF_CHECK(Tests, !Visibility.IsEffectivelyVisible(Child, ParentLookup));
		Visibility.SetVisible(Child, true);
		PF_CHECK(Tests, Visibility.IsEffectivelyVisible(Child, ParentLookup));

		Visibility.SetVisible(Sibling, false);
		const std::array<UUID, 2> Keep{ Parent, Child };
		const std::array<UUID, 3> All{ Parent, Child, Sibling };
		PF_CHECK(Tests, Visibility.Isolate(Keep, All));
		PF_CHECK(Tests, Visibility.IsLocallyVisible(Parent) && Visibility.IsLocallyVisible(Child));
		PF_CHECK(Tests, !Visibility.IsLocallyVisible(Sibling));
		PF_CHECK(Tests, Visibility.RestoreIsolation());
		PF_CHECK(Tests, !Visibility.IsLocallyVisible(Sibling));
		Visibility.ShowAll();
		PF_CHECK(Tests, Visibility.IsLocallyVisible(Sibling) && !Visibility.GetIsolationBaseline());

		PF_CHECK(Tests, Visibility.IsEffectivelyVisible(Parent, [Parent, Child](const UUID& Identifier)
		{
			return Identifier == Parent ? std::optional<UUID>{ Child } : std::optional<UUID>{ Parent };
		}) == false);
	}

	void TestEditorHierarchyRowLayout(TestRunner& Tests)
	{
		using namespace PulseForgeEditor;
		constexpr float FramePaddingX = 7.0f;
		constexpr float TextStartX = 50.0f;
		constexpr float TreeTrailingHitWidth = FramePaddingX;

		const auto WideRow = ComputeEditorHierarchyRowLayout(
			8.0f, 240.0f, TextStartX, 100.0f, 108.0f, FramePaddingX);
		PF_CHECK(Tests, WideRow.EyeStartX == 210.0f);
		PF_CHECK(Tests, WideRow.EyeStartX >= 100.0f + EditorHierarchyHitRegionGap);
		PF_CHECK(Tests, WideRow.EyeStartX + EditorHierarchyEyeButtonWidth <= 240.0f);
		PF_CHECK(Tests, WideRow.MaximumNameWidth > 80.0f);
		PF_CHECK(Tests, WideRow.SelectionGutterStartX == 108.0f && WideRow.SelectionGutterWidth > 0.0f);
		PF_CHECK(Tests, WideRow.SelectionGutterStartX >= 100.0f);
		PF_CHECK(Tests, WideRow.SelectionGutterStartX + WideRow.SelectionGutterWidth <=
			WideRow.EyeStartX - EditorHierarchyHitRegionGap);
		PF_CHECK(Tests, 8.0f + WideRow.SelectionBackgroundWidth ==
			WideRow.EyeStartX - EditorHierarchyHitRegionGap);
		PF_CHECK(Tests, WideRow.SelectionGutterStartX + WideRow.SelectionGutterWidth ==
			8.0f + WideRow.SelectionBackgroundWidth);
		const float LongNameTreeItemRightX = TextStartX + WideRow.MaximumNameWidth + TreeTrailingHitWidth;
		PF_CHECK(Tests, LongNameTreeItemRightX <= WideRow.EyeStartX - EditorHierarchyHitRegionGap -
			EditorHierarchyHitRegionSafetyInset);

		const auto NarrowRow = ComputeEditorHierarchyRowLayout(
			8.0f, 150.0f, 50.0f, 94.0f, 102.0f, FramePaddingX);
		PF_CHECK(Tests, NarrowRow.EyeStartX >= 94.0f + EditorHierarchyHitRegionGap);
		PF_CHECK(Tests, NarrowRow.EyeStartX + EditorHierarchyEyeButtonWidth <= 150.0f);
		PF_CHECK(Tests, NarrowRow.MaximumNameWidth >= 0.0f && NarrowRow.SelectionGutterWidth >= 0.0f);
		PF_CHECK(Tests, NarrowRow.SelectionGutterStartX >= 94.0f);
		PF_CHECK(Tests, NarrowRow.SelectionGutterStartX + NarrowRow.SelectionGutterWidth <=
			NarrowRow.EyeStartX - EditorHierarchyHitRegionGap);
		PF_CHECK(Tests, 8.0f + NarrowRow.SelectionBackgroundWidth ==
			NarrowRow.EyeStartX - EditorHierarchyHitRegionGap);

		const auto DeepRow = ComputeEditorHierarchyRowLayout(
			98.0f, 240.0f, 140.0f, 175.0f, 183.0f, FramePaddingX);
		PF_CHECK(Tests, DeepRow.EyeStartX == 210.0f);
		PF_CHECK(Tests, DeepRow.SelectionGutterStartX >= 175.0f);
		PF_CHECK(Tests, DeepRow.SelectionGutterStartX + DeepRow.SelectionGutterWidth <=
			DeepRow.EyeStartX - EditorHierarchyHitRegionGap);
		PF_CHECK(Tests, 98.0f + DeepRow.SelectionBackgroundWidth ==
			DeepRow.EyeStartX - EditorHierarchyHitRegionGap);

		const auto ConstrainedDeepRow = ComputeEditorHierarchyRowLayout(
			170.0f, 260.0f, 215.0f, 230.0f, 238.0f, FramePaddingX);
		PF_CHECK(Tests, ConstrainedDeepRow.EyeStartX >= 234.0f);
		PF_CHECK(Tests, ConstrainedDeepRow.EyeStartX + EditorHierarchyEyeButtonWidth <= 260.0f);
		PF_CHECK(Tests, ConstrainedDeepRow.SelectionGutterWidth == 0.0f);
		PF_CHECK(Tests, ConstrainedDeepRow.EyeStartX >= 230.0f + EditorHierarchyHitRegionGap);
		PF_CHECK(Tests, 170.0f + ConstrainedDeepRow.SelectionBackgroundWidth ==
			ConstrainedDeepRow.EyeStartX - EditorHierarchyHitRegionGap);
	}

	void TestSceneCommandHistoryRestoresIdentity(TestRunner& Tests)
	{
		using namespace PulseForge;
		Scene SceneValue;
		const auto Root = SceneValue.CreateEntity("Parent");
		PF_CHECK(Tests, Root.has_value());
		if (!Root)
			return;

		PulseForgeEditor::EditorCommandHistory History;
		History.Reset();
		const auto Before = SceneSerializer::Serialize(SceneValue);
		PF_CHECK(Tests, Before.has_value());
		if (!Before)
			return;
		PF_CHECK(Tests, History.Begin("Create child", *Before));

		auto Child = SceneValue.CreateEntity("Shadow Caster");
		PF_CHECK(Tests, Child.has_value());
		if (!Child)
			return;
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
		if (!After)
			return;
		PF_CHECK(Tests, History.Commit(*After));

		auto Restore = [&SceneValue](std::string_view State) -> std::expected<void, std::string>
		{
			if (auto Result = SceneSerializer::Deserialize(State, SceneValue); !Result)
				return std::unexpected(Result.error().Message);
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

	void TestEditorSceneEditController(TestRunner& Tests)
	{
		using namespace PulseForge;
		using namespace PulseForgeEditor;

		Scene SceneValue;
		EditorSceneWorkspaceState Workspace;
		EditorSceneEditController Controller;
		auto Created = SceneValue.CreateEntity("Controller target");
		PF_CHECK(Tests, Created.has_value());
		if (!Created)
			return;
		const UUID Identifier = Created->GetUUID();
		Workspace.Selection().SelectSingle(Identifier);
		TransformComponent Initial;
		Initial.Translation = { 1.0f, 2.0f, 3.0f };
		PF_CHECK(Tests, Created->SetTransform(Initial));
		Controller.Reset(&SceneValue);

		size_t RestoreCount = 0;
		const EditorSceneEditController::RestoreCallback Restore =
			[&](std::string_view State) -> std::expected<void, std::string>
		{
			++RestoreCount;
			return Controller.Restore(State, SceneValue, Workspace);
		};
		const auto SetTransform = [&](const TransformComponent& Value) -> std::expected<void, SceneError>
		{
			const auto Target = SceneValue.FindEntity(Identifier);
			if (!Target)
				return std::unexpected(SceneError{ SceneErrorCode::InvalidEntity, "Edit target no longer exists" });
			return Target->SetTransform(Value);
		};
		const auto ReadTranslation = [&]() -> std::optional<glm::vec3>
		{
			const auto Target = SceneValue.FindEntity(Identifier);
			if (!Target)
				return std::nullopt;
			const auto Value = Target->GetTransform();
			return Value ? std::optional<glm::vec3>{ Value->Translation } : std::nullopt;
		};

		TransformComponent FirstEdit = Initial;
		FirstEdit.Translation.x = 4.0f;
		const auto First = Controller.Run(
			&SceneValue, Workspace, false, 0, [] { return 0u; }, "Move target",
			[&] { return SetTransform(FirstEdit); }, Restore);
		PF_CHECK(Tests, First && !Controller.IsActive() && Controller.CanUndo() && Controller.IsDirty());
		PF_CHECK(Tests, ReadTranslation() == std::optional<glm::vec3>{ FirstEdit.Translation });
		PF_CHECK(Tests, Workspace.Selection().GetPrimary() == std::optional<UUID>{ Identifier });

		const auto Undone = Controller.Undo(&SceneValue, Workspace, false, Restore);
		PF_CHECK(Tests, Undone && *Undone && !Controller.IsDirty());
		PF_CHECK(Tests, ReadTranslation() == std::optional<glm::vec3>{ Initial.Translation });
		const auto Redone = Controller.Redo(&SceneValue, Workspace, false, Restore);
		PF_CHECK(Tests, Redone && *Redone && Controller.IsDirty());
		PF_CHECK(Tests, ReadTranslation() == std::optional<glm::vec3>{ FirstEdit.Translation });
		Controller.MarkSaved(&SceneValue);
		PF_CHECK(Tests, !Controller.IsDirty());

		uint32_t ActiveItem = 41;
		TransformComponent GroupedEdit = FirstEdit;
		GroupedEdit.Translation.y = 8.0f;
		const auto GroupStart = Controller.Run(
			&SceneValue, Workspace, false, ActiveItem, [&] { return ActiveItem; }, "Adjust target",
			[&] { return SetTransform(GroupedEdit); }, Restore);
		PF_CHECK(Tests, GroupStart && Controller.IsActive() && !Controller.CanUndo());
		ActiveItem = 42;
		TransformComponent NextEdit = GroupedEdit;
		NextEdit.Translation.z = 9.0f;
		const auto GroupBoundary = Controller.Run(
			&SceneValue, Workspace, false, ActiveItem, [] { return 0u; }, "Adjust target",
			[&] { return SetTransform(NextEdit); }, Restore);
		PF_CHECK(Tests, GroupBoundary && !Controller.IsActive() && Controller.CanUndo());
		PF_CHECK(Tests, ReadTranslation() == std::optional<glm::vec3>{ NextEdit.Translation });
		const auto UndoSecond = Controller.Undo(&SceneValue, Workspace, false, Restore);
		PF_CHECK(Tests, UndoSecond && *UndoSecond && ReadTranslation() ==
			std::optional<glm::vec3>{ GroupedEdit.Translation });
		const auto UndoFirst = Controller.Undo(&SceneValue, Workspace, false, Restore);
		PF_CHECK(Tests, UndoFirst && *UndoFirst && ReadTranslation() ==
			std::optional<glm::vec3>{ FirstEdit.Translation });

		Controller.Reset(&SceneValue);
		TransformComponent FailedEdit = FirstEdit;
		FailedEdit.Translation = { 13.0f, 14.0f, 15.0f };
		const auto Failed = Controller.Run(
			&SceneValue, Workspace, false, 0, [] { return 0u; }, "Failing edit",
			[&]() -> std::expected<void, SceneError>
			{
				if (auto Updated = SetTransform(FailedEdit); !Updated)
					return Updated;
				return std::unexpected(SceneError{ SceneErrorCode::InvalidEntity, "Injected edit failure" });
			}, Restore);
		PF_CHECK(Tests, !Failed && !Controller.IsActive() && !Controller.IsDirty() && !Controller.CanUndo());
		PF_CHECK(Tests, ReadTranslation() == std::optional<glm::vec3>{ FirstEdit.Translation });
		PF_CHECK(Tests, RestoreCount >= 4);
		const auto RuntimeEdit = Controller.Run(
			&SceneValue, Workspace, true, 0, [] { return 0u; }, "Runtime edit",
			[&] { return SetTransform(Initial); }, Restore);
		PF_CHECK(Tests, !RuntimeEdit && !Controller.IsActive());
	}

	void TestEditorAssetPreviewPolicy(TestRunner& Tests)
	{
		using namespace PulseForgeEditor;
		const AssetPreviewDimensions Wide = CalculateAssetPreviewDimensions(4096, 2048, 192);
		PF_CHECK(Tests, Wide.Width == 192 && Wide.Height == 96);
		const AssetPreviewDimensions Tall = CalculateAssetPreviewDimensions(64, 4096, 192);
		PF_CHECK(Tests, Tall.Width == 3 && Tall.Height == 192);
		const AssetPreviewDimensions Tiny = CalculateAssetPreviewDimensions(3, 2, 192);
		PF_CHECK(Tests, Tiny.Width == 3 && Tiny.Height == 2);
		PF_CHECK(Tests, CalculateAssetPreviewDimensions(0, 4, 192).Width == 0);
		PF_CHECK(Tests, CalculateAssetPreviewDimensions(4, 4, 0).Height == 0);

		const AssetPreviewRect Fitted = FitAssetPreview(10.0f, 20.0f, 100.0f, 60.0f, 200, 100);
		PF_CHECK(Tests, std::isfinite(Fitted.X) && std::isfinite(Fitted.Y) &&
			std::isfinite(Fitted.Width) && std::isfinite(Fitted.Height));
		PF_CHECK(Tests, Fitted.Width == 100.0f && Fitted.Height == 50.0f && Fitted.Y == 25.0f);
		const AssetPreviewRect Invalid = FitAssetPreview(0.0f, 0.0f, -1.0f, 10.0f, 1, 1);
		PF_CHECK(Tests, Invalid.Width == 0.0f && Invalid.Height == 0.0f);

		AssetPreviewKey First{ "project-a", "asset", { 1, 2 }, AssetPreviewKind::Image, 192, 96, 1 };
		AssetPreviewKey Same = First;
		AssetPreviewKey OtherProject = First;
		OtherProject.ProjectID = "project-b";
		AssetPreviewKey OtherRevision = First;
		OtherRevision.ContentRevision[1]++;
		PF_CHECK(Tests, First == Same && AssetPreviewKeyHash{}(First) == AssetPreviewKeyHash{}(Same));
		PF_CHECK(Tests, !(First == OtherProject) && !(First == OtherRevision));
		const auto OpaqueSwatch = MakeMaterialBaseColorSwatch(0.5f, 1.0f, 0.0f, 0.2f, true);
		PF_CHECK(Tests, OpaqueSwatch[0] == 188 && OpaqueSwatch[1] == 255 && OpaqueSwatch[2] == 0 && OpaqueSwatch[3] == 255);
		const auto BlendSwatch = MakeMaterialBaseColorSwatch(-1.0f, 0.0f, 0.0f, 0.25f, false);
		PF_CHECK(Tests, BlendSwatch[0] == 0 && BlendSwatch[3] == 64);

		AssetPreviewBudget Budget(100);
		PF_CHECK(Tests, Budget.Add(60) && Budget.GetUsed() == 60);
		PF_CHECK(Tests, !Budget.Add(41) && Budget.GetUsed() == 60);
		Budget.Remove(20);
		PF_CHECK(Tests, Budget.CanFit(60) && !Budget.CanFit(81));
		Budget.SetLimit(0);
		Budget.Clear();
		PF_CHECK(Tests, !Budget.Add(1) && Budget.Add(0));
		PF_CHECK(Tests, IsSupportedImagePreviewExtension("sample.PNG"));
		PF_CHECK(Tests, IsSupportedImagePreviewExtension("sample.ppm"));
		PF_CHECK(Tests, !IsSupportedImagePreviewExtension("sample.hdr"));
		PF_CHECK(Tests, !IsSupportedImagePreviewExtension("sample.material"));

		bool Visible = false;
		uint64_t LastUse = 0;
		uint64_t UseCounter = 0;
		TouchAssetPreviewEntry(Visible, LastUse, UseCounter);
		PF_CHECK(Tests, Visible && LastUse == 1);
		BeginAssetPreviewFrame(Visible);
		PF_CHECK(Tests, !Visible);
		TouchAssetPreviewEntry(Visible, LastUse, UseCounter);
		PF_CHECK(Tests, Visible && LastUse == 2);
		std::array<bool, 4> StateVisibility{}; // Queued, Decoding, Ready, and Failed entries use the same touch path.
		for (bool& StateVisible : StateVisibility)
		{
			BeginAssetPreviewFrame(StateVisible);
			TouchAssetPreviewEntry(StateVisible, LastUse, UseCounter);
		}
		PF_CHECK(Tests, std::ranges::all_of(StateVisibility, [](bool StateVisible) { return StateVisible; }));

		const std::array<AssetPreviewEvictionCandidate, 3> VisibilityCandidates = {{
			{ "ready", true, false, LastUse, 8 },
			{ "queued", true, true, 1, 0 },
			{ "unused", false, false, 0, 8 }
		}};
		const auto ProtectedVisible = PlanAssetPreviewEvictions(
			VisibilityCandidates, 3, 2, 16, 16, 0, 0);
		PF_CHECK(Tests, ProtectedVisible && ProtectedVisible->size() == 1 && (*ProtectedVisible)[0] == "unused");
		const std::array<AssetPreviewEvictionCandidate, 2> FullCacheCandidates = {{
			{ "old-cache-entry", false, false, 1, 0 },
			{ "visible-cache-entry", true, false, 2, 0 }
		}};
		const auto FullEntryCache = PlanAssetPreviewEvictions(
			FullCacheCandidates, 2, 2, 0, 0, 0, 1);
		PF_CHECK(Tests, FullEntryCache && FullEntryCache->size() == 1 && (*FullEntryCache)[0] == "old-cache-entry");

		const std::array<AssetPreviewEvictionCandidate, 3> UploadCandidates = {{
			{ "uploading", false, false, 1, 4 },
			{ "recent-unused", false, false, 9, 8 },
			{ "old-unused", false, false, 2, 8 }
		}};
		const auto UploadEviction = PlanAssetPreviewEvictions(
			UploadCandidates, 3, 3, 20, 20, 12, 0, "uploading");
		PF_CHECK(Tests, UploadEviction && UploadEviction->size() == 2 &&
			(*UploadEviction)[0] == "old-unused" && (*UploadEviction)[1] == "recent-unused");
		const auto NoUploadVictim = PlanAssetPreviewEvictions(
			UploadCandidates, 3, 3, 20, 20, 17, 0, "uploading");
		PF_CHECK(Tests, !NoUploadVictim);

		const std::array<AssetPreviewEvictionCandidate, 2> BusyCache = {{
			{ "visible", true, false, 4, 10 },
			{ "decoding", false, true, 1, 0 }
		}};
		PF_CHECK(Tests, !PlanAssetPreviewEvictions(BusyCache, 2, 2, 10, 10, 1, 0));
		const auto StaleQueuedResult = [](uint64_t Work, uint64_t Entry, uint64_t Current, bool Stopping)
		{
			return IsCurrentAssetPreviewResult(Work, Entry, Current, Stopping);
		};
		PF_CHECK(Tests, StaleQueuedResult(5, 5, 5, false));
		PF_CHECK(Tests, !StaleQueuedResult(4, 4, 5, false));
		PF_CHECK(Tests, !StaleQueuedResult(5, 4, 5, false));
		PF_CHECK(Tests, !StaleQueuedResult(5, 5, 5, true));

		const std::array<AssetPreviewEvictionCandidate, 1> AfterEviction = {{
			{ "remaining", true, false, 5, 0 }
		}};
		const auto ReRequest = PlanAssetPreviewEvictions(AfterEviction, 1, 2, 0, 0, 0, 1);
		PF_CHECK(Tests, ReRequest && ReRequest->empty());
	}
}

void RunEditorHistoryTests(TestRunner& Tests)
{
	TestEditorInteractionPolicy(Tests);
	TestEditorAssetPreviewPolicy(Tests);
	TestConsoleFormatting(Tests);
	TestEditorCommandHistory(Tests);
	TestAltGizmoDragHistoryIntegration(Tests);
	TestEditorSelection(Tests);
	TestEditorVisibility(Tests);
	TestEditorHierarchyRowLayout(Tests);
	TestSceneCommandHistoryRestoresIdentity(Tests);
	TestEditorSceneEditController(Tests);
}
