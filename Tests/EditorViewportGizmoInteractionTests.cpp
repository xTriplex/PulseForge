#include "TestSupport.h"

#include "Editor/EditorSceneEditController.h"
#include "Editor/EditorSceneWorkspaceState.h"
#include "Editor/EditorViewportGizmoInteraction.h"
#include "Scene/Scene.h"

#include <glm/gtc/matrix_transform.hpp>

void RunEditorViewportGizmoInteractionTests(TestRunner& Tests)
{
	using namespace PulseForge;
	using namespace PulseForgeEditor;

	Scene SceneValue;
	auto Entity = SceneValue.CreateEntity("Gizmo target");
	PF_CHECK(Tests, Entity.has_value());
	if (!Entity)
		return;

	EditorSceneWorkspaceState Workspace;
	Workspace.Selection().SelectSingle(Entity->GetUUID());
	EditorSceneEditController Edits;
	Edits.Reset(&SceneValue);
	EditorViewportGizmoInteraction Interaction;
	const glm::mat4 View = glm::lookAtRH(glm::vec3(0.0f, 0.0f, 5.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
	const glm::mat4 Projection = glm::perspectiveRH_ZO(glm::radians(45.0f), 1.0f, 0.1f, 100.0f);
	const glm::mat4 ViewProjection = Projection * View;
	const ViewportImageRect ImageRect{ { 0.0f, 0.0f }, { 100.0f, 100.0f } };
	ViewportGizmoGeometry Geometry;
	Geometry.IsValid = true;
	Geometry.PivotWorld = glm::vec3(0.0f);
	Geometry.Axes[0] = { .WorldDirection = { 1.0f, 0.0f, 0.0f }, .WorldUnitsPerLocalUnit = 1.0f, .IsValid = true };
	const auto Restore = [&Edits, &SceneValue, &Workspace](std::string_view Snapshot)
		-> std::expected<void, std::string>
	{
		return Edits.Restore(Snapshot, SceneValue, Workspace);
	};
	const auto BeginInput = [&](bool Duplicate)
	{
		EditorViewportGizmoBeginInput Input;
		Input.Handle = { TransformGizmoOperation::Translate, 0 };
		Input.Geometry = Geometry;
		Input.MousePosition = { 50.0f, 50.0f };
		Input.ImageRect = ImageRect;
		Input.ViewProjection = ViewProjection;
		Input.CameraPosition = { 0.0f, 0.0f, 5.0f };
		Input.ViewProjectionValid = true;
		Input.DuplicateOnDrag = Duplicate;
		return Input;
	};
	const auto UpdateInput = [&](float X, bool Alt)
	{
		EditorViewportGizmoUpdateInput Input;
		Input.MousePosition = { X, 50.0f };
		Input.ImageRect = ImageRect;
		Input.ViewProjection = ViewProjection;
		Input.AltDown = Alt;
		return Input;
	};
	const auto InvalidGeometryInput = BeginInput(false);
	auto InvalidGeometry = InvalidGeometryInput;
	InvalidGeometry.Geometry.IsValid = false;
	const auto InvalidBegin = Interaction.Begin(SceneValue, Workspace, Edits, InvalidGeometry, Restore);
	PF_CHECK(Tests, InvalidBegin && !*InvalidBegin && !Interaction.IsActive() && !Edits.IsActive());

	const auto Began = Interaction.Begin(SceneValue, Workspace, Edits, BeginInput(false), Restore);
	PF_CHECK(Tests, Began && *Began && Interaction.IsActive() && Edits.IsActive());
	if (!Began || !*Began)
		return;
	PF_CHECK(Tests, Interaction.Update(&SceneValue, Workspace, Edits, UpdateInput(75.0f, false), Restore));
	const auto Moved = Entity->GetTransform();
	PF_CHECK(Tests, Moved && Moved->Translation.x > 0.1f && Edits.IsDirty());
	PF_CHECK(Tests, Interaction.Complete(&SceneValue, Workspace, Edits, Restore));
	PF_CHECK(Tests, !Interaction.IsActive() && Edits.CanUndo());
	const auto Undone = Edits.Undo(&SceneValue, Workspace, false, Restore);
	PF_CHECK(Tests, Undone && *Undone);
	const auto Restored = SceneValue.FindEntity(Entity->GetUUID());
	PF_CHECK(Tests, Restored.has_value());
	if (!Restored)
		return;
	const auto RestoredTransform = Restored->GetTransform();
	PF_CHECK(Tests, RestoredTransform && glm::length(RestoredTransform->Translation) < 1.0e-5f);

	const auto BeganCancel = Interaction.Begin(SceneValue, Workspace, Edits, BeginInput(false), Restore);
	PF_CHECK(Tests, BeganCancel && *BeganCancel);
	if (!BeganCancel || !*BeganCancel)
		return;
	PF_CHECK(Tests, Interaction.Update(&SceneValue, Workspace, Edits, UpdateInput(72.0f, false), Restore));
	PF_CHECK(Tests, Interaction.Cancel(&SceneValue, Workspace, Edits, Restore));
	PF_CHECK(Tests, !Interaction.IsActive() && !Edits.IsActive());
	const auto Cancelled = SceneValue.FindEntity(Entity->GetUUID());
	PF_CHECK(Tests, Cancelled.has_value());
	if (!Cancelled)
		return;
	const auto CancelledTransform = Cancelled->GetTransform();
	PF_CHECK(Tests, CancelledTransform && glm::length(CancelledTransform->Translation) < 1.0e-5f);

	const auto BeganFailedCancel = Interaction.Begin(SceneValue, Workspace, Edits, BeginInput(false), Restore);
	PF_CHECK(Tests, BeganFailedCancel && *BeganFailedCancel);
	if (!BeganFailedCancel || !*BeganFailedCancel)
		return;
	PF_CHECK(Tests, Interaction.Update(&SceneValue, Workspace, Edits, UpdateInput(75.0f, false), Restore));
	const auto FailingRestore = [](std::string_view) -> std::expected<void, std::string>
	{
		return std::unexpected("Injected scene restore failure");
	};
	const auto FailedCancel = Interaction.Cancel(&SceneValue, Workspace, Edits, FailingRestore);
	PF_CHECK(Tests, !FailedCancel && Interaction.IsActive() && Edits.IsActive());
	const auto PendingRollback = SceneValue.FindEntity(Entity->GetUUID());
	PF_CHECK(Tests, PendingRollback.has_value());
	if (!PendingRollback)
		return;
	const auto PendingTransform = PendingRollback->GetTransform();
	PF_CHECK(Tests, PendingTransform && PendingTransform->Translation.x > 0.1f);
	PF_CHECK(Tests, Interaction.Cancel(&SceneValue, Workspace, Edits, Restore));
	PF_CHECK(Tests, !Interaction.IsActive() && !Edits.IsActive());
	const auto RetriedRollback = SceneValue.FindEntity(Entity->GetUUID());
	PF_CHECK(Tests, RetriedRollback.has_value());
	if (!RetriedRollback)
		return;
	const auto RetriedTransform = RetriedRollback->GetTransform();
	PF_CHECK(Tests, RetriedTransform && glm::length(RetriedTransform->Translation) < 1.0e-5f);

	auto OtherEntity = SceneValue.CreateEntity("Other selection");
	PF_CHECK(Tests, OtherEntity.has_value());
	if (!OtherEntity)
		return;
	const auto BeganSelectionChange = Interaction.Begin(SceneValue, Workspace, Edits, BeginInput(false), Restore);
	PF_CHECK(Tests, BeganSelectionChange && *BeganSelectionChange);
	if (!BeganSelectionChange || !*BeganSelectionChange)
		return;
	PF_CHECK(Tests, Interaction.Update(&SceneValue, Workspace, Edits, UpdateInput(72.0f, false), Restore));
	Workspace.Selection().SelectSingle(OtherEntity->GetUUID());
	PF_CHECK(Tests, Interaction.Update(&SceneValue, Workspace, Edits, UpdateInput(75.0f, false), Restore));
	const auto SelectionChangeRestored = SceneValue.FindEntity(Entity->GetUUID());
	PF_CHECK(Tests, SelectionChangeRestored.has_value());
	if (!SelectionChangeRestored)
		return;
	const auto SelectionChangeTransform = SelectionChangeRestored->GetTransform();
	PF_CHECK(Tests, !Interaction.IsActive() && !Edits.IsActive() && SelectionChangeTransform &&
		glm::length(SelectionChangeTransform->Translation) < 1.0e-5f);

	const auto BeganDuplicate = Interaction.Begin(SceneValue, Workspace, Edits, BeginInput(true), Restore);
	PF_CHECK(Tests, BeganDuplicate && *BeganDuplicate);
	if (!BeganDuplicate || !*BeganDuplicate)
		return;
	PF_CHECK(Tests, Interaction.Update(&SceneValue, Workspace, Edits, UpdateInput(75.0f, true), Restore));
	const auto Duplicate = Workspace.Selection().GetPrimary();
	PF_CHECK(Tests, Duplicate && *Duplicate != Entity->GetUUID() && SceneValue.FindEntity(*Duplicate).has_value());
	if (!Duplicate)
		return;
	PF_CHECK(Tests, Interaction.Complete(&SceneValue, Workspace, Edits, Restore));
	const auto DuplicateUndo = Edits.Undo(&SceneValue, Workspace, false, Restore);
	PF_CHECK(Tests, DuplicateUndo && *DuplicateUndo && !SceneValue.FindEntity(*Duplicate).has_value());
	PF_CHECK(Tests, Workspace.Selection().GetPrimary() == Entity->GetUUID());
}
