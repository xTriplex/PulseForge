#include "TestSupport.h"

#include "Editor/EditorSceneEditController.h"
#include "Editor/EditorSceneWorkspaceState.h"
#include "Editor/EditorViewportGizmoInteraction.h"
#include "Scene/Scene.h"

#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <optional>

namespace
{
	using namespace PulseForge;
	using namespace PulseForgeEditor;

	ViewportGizmoGeometry MakeTestGeometry();

	bool SameTransform(const TransformComponent& First, const TransformComponent& Second)
	{
		constexpr float Epsilon = 1.0e-5f;
		return glm::length(First.Translation - Second.Translation) <= Epsilon && glm::length(First.Scale - Second.Scale) <= Epsilon &&
			   1.0f - std::abs(glm::dot(glm::normalize(First.Rotation), glm::normalize(Second.Rotation))) <= Epsilon;
	}

	void RunLocalReturnToOriginCase(TestRunner& Tests, TransformGizmoOperation Operation, uint32_t Axis, glm::vec2 StartMouse,
		glm::vec2 MovedMouse, glm::vec2 SnappedReturnMouse)
	{
		Scene SceneValue;
		auto Entity = SceneValue.CreateEntity("Return-to-origin target");
		PF_CHECK(Tests, Entity.has_value());
		if (!Entity) return;

		EditorSceneWorkspaceState Workspace;
		Workspace.Selection().SelectSingle(Entity->GetUUID());
		EditorSceneEditController Edits;
		Edits.Reset(&SceneValue);
		EditorViewportGizmoInteraction Interaction;
		const glm::mat4 View = glm::lookAtRH(glm::vec3(0.0f, 0.0f, 5.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
		const glm::mat4 Projection = glm::perspectiveRH_ZO(glm::radians(45.0f), 1.0f, 0.1f, 100.0f);
		const glm::mat4 ViewProjection = Projection * View;
		const ViewportImageRect ImageRect{ { 0.0f, 0.0f }, { 100.0f, 100.0f } };
		constexpr std::array<glm::vec3, 3> AxisDirections = { glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f),
			glm::vec3(0.0f, 0.0f, 1.0f) };
		ViewportGizmoGeometry Geometry;
		Geometry.IsValid = true;
		for (size_t Index = 0; Index < AxisDirections.size(); ++Index)
			Geometry.Axes[Index] = { .WorldDirection = AxisDirections[Index], .WorldUnitsPerLocalUnit = 1.0f, .IsValid = true };

		const auto Restore = [&Edits, &SceneValue, &Workspace](std::string_view Snapshot) -> std::expected<void, std::string>
		{ return Edits.Restore(Snapshot, SceneValue, Workspace); };
		const auto MakeBeginInput = [&]
		{
			EditorViewportGizmoBeginInput Input;
			Input.Handle = { Operation, Axis };
			Input.Geometry = Geometry;
			Input.MousePosition = StartMouse;
			Input.ImageRect = ImageRect;
			Input.ViewProjection = ViewProjection;
			Input.CameraPosition = { 0.0f, 0.0f, 5.0f };
			Input.ViewProjectionValid = true;
			return Input;
		};
		const auto MakeUpdateInput = [&](glm::vec2 MousePosition)
		{
			EditorViewportGizmoUpdateInput Input;
			Input.MousePosition = MousePosition;
			Input.ImageRect = ImageRect;
			Input.ViewProjection = ViewProjection;
			Input.ToolValues.TransformSnapping = true;
			Input.ToolValues.LocalSpace = true;
			return Input;
		};
		const auto GetTransform = [&]() -> std::optional<TransformComponent>
		{
			const auto Current = SceneValue.FindEntity(Entity->GetUUID());
			if (!Current) return std::nullopt;
			const auto Transform = Current->GetTransform();
			return Transform ? std::optional<TransformComponent>(*Transform) : std::nullopt;
		};
		const TransformComponent Initial = *Entity->GetTransform();

		const auto Began = Interaction.Begin(SceneValue, Workspace, Edits, MakeBeginInput(), Restore);
		PF_CHECK(Tests, Began && *Began);
		if (!Began || !*Began) return;
		PF_CHECK(Tests, Interaction.Update(&SceneValue, Workspace, Edits, MakeUpdateInput(MovedMouse), Restore));
		const auto Displaced = GetTransform();
		PF_CHECK(Tests, Displaced.has_value() && !SameTransform(*Displaced, Initial));
		PF_CHECK(Tests, Edits.IsDirty());
		PF_CHECK(Tests, Interaction.Update(&SceneValue, Workspace, Edits, MakeUpdateInput(StartMouse), Restore));
		const auto ReturnedToPointerOrigin = GetTransform();
		PF_CHECK(Tests, ReturnedToPointerOrigin.has_value() && SameTransform(*ReturnedToPointerOrigin, Initial));
		PF_CHECK(Tests, Interaction.Update(&SceneValue, Workspace, Edits, MakeUpdateInput(MovedMouse), Restore));
		PF_CHECK(Tests, Interaction.Update(&SceneValue, Workspace, Edits, MakeUpdateInput(SnappedReturnMouse), Restore));
		const auto Returned = GetTransform();
		PF_CHECK(Tests, Returned.has_value() && SameTransform(*Returned, Initial));
		PF_CHECK(Tests, Interaction.Complete(&SceneValue, Workspace, Edits, Restore));
		PF_CHECK(Tests, !Interaction.IsActive() && !Edits.IsActive());
		PF_CHECK(Tests, !Edits.IsDirty() && !Edits.CanUndo() && !Edits.CanRedo());

		const auto BeganAgain = Interaction.Begin(SceneValue, Workspace, Edits, MakeBeginInput(), Restore);
		PF_CHECK(Tests, BeganAgain && *BeganAgain);
		if (!BeganAgain || !*BeganAgain) return;
		PF_CHECK(Tests, Interaction.Update(&SceneValue, Workspace, Edits, MakeUpdateInput(MovedMouse), Restore));
		PF_CHECK(Tests, Interaction.Update(&SceneValue, Workspace, Edits, MakeUpdateInput(SnappedReturnMouse), Restore));
		PF_CHECK(Tests, Interaction.Cancel(&SceneValue, Workspace, Edits, Restore));
		const auto Cancelled = GetTransform();
		PF_CHECK(Tests, Cancelled.has_value() && SameTransform(*Cancelled, Initial));
		PF_CHECK(Tests, !Interaction.IsActive());
		PF_CHECK(Tests, !Edits.IsActive());
		PF_CHECK(Tests, !Edits.IsDirty());
		PF_CHECK(Tests, !Edits.CanUndo());
	}

	void RunParentChildLocalTransformCase(TestRunner& Tests, TransformGizmoOperation Operation)
	{
		using namespace PulseForge;
		using namespace PulseForgeEditor;
		Scene SceneValue;
		auto Parent = SceneValue.CreateEntity("Selected parent root");
		auto Child = SceneValue.CreateEntity("Primary selected child");
		PF_CHECK(Tests, Parent && Child);
		if (!Parent || !Child) return;

		TransformComponent ParentInitial = *Parent->GetTransform();
		ParentInitial.Rotation = glm::angleAxis(glm::radians(45.0f), glm::vec3(0.0f, 0.0f, 1.0f));
		ParentInitial.Scale = { 2.0f, 1.0f, 1.0f };
		PF_CHECK(Tests, Parent->SetTransform(ParentInitial));
		PF_CHECK(Tests, Child->SetParent(*Parent));
		TransformComponent ChildInitial = *Child->GetTransform();
		ChildInitial.Translation = { 0.5f, 0.25f, 0.0f };
		ChildInitial.Rotation = glm::angleAxis(glm::radians(75.0f), glm::vec3(0.0f, 0.0f, 1.0f));
		ChildInitial.Scale = { 0.5f, 1.25f, 0.75f };
		PF_CHECK(Tests, Child->SetTransform(ChildInitial));

		EditorSceneWorkspaceState Workspace;
		Workspace.Selection().Set({ Parent->GetUUID(), Child->GetUUID() }, Child->GetUUID());
		EditorSceneEditController Edits;
		Edits.Reset(&SceneValue);
		EditorViewportGizmoInteraction Interaction;
		const glm::mat4 View = glm::lookAtRH(glm::vec3(0.0f, 0.0f, 5.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
		const glm::mat4 Projection = glm::perspectiveRH_ZO(glm::radians(45.0f), 1.0f, 0.1f, 100.0f);
		const glm::mat4 ViewProjection = Projection * View;
		const ViewportImageRect ImageRect{ { 0.0f, 0.0f }, { 100.0f, 100.0f } };
		const glm::vec3 AxisX = glm::normalize(ParentInitial.Rotation * glm::vec3(1.0f, 0.0f, 0.0f));
		ViewportGizmoGeometry Geometry = MakeTestGeometry();
		Geometry.PivotWorld = { 0.0f, 0.0f, 0.0f };
		Geometry.Axes[0].WorldDirection = AxisX;
		Geometry.Axes[0].WorldUnitsPerLocalUnit = 2.0f;
		Geometry.Axes[2].WorldDirection = { 0.0f, 0.0f, 1.0f };
		const uint32_t Axis = Operation == TransformGizmoOperation::Rotate ? 2u : 0u;
		const glm::vec2 StartMouse = Operation == TransformGizmoOperation::Rotate ? glm::vec2{ 75.0f, 50.0f } : glm::vec2{ 50.0f, 50.0f };
		const glm::vec2 MovedMouse = Operation == TransformGizmoOperation::Rotate ? glm::vec2{ 50.0f, 75.0f } : glm::vec2{ 75.0f, 25.0f };
		EditorViewportGizmoBeginInput BeginInput;
		BeginInput.Handle = { Operation, Axis };
		BeginInput.Geometry = Geometry;
		BeginInput.MousePosition = StartMouse;
		BeginInput.ImageRect = ImageRect;
		BeginInput.ViewProjection = ViewProjection;
		BeginInput.CameraPosition = { 0.0f, 0.0f, 5.0f };
		BeginInput.ViewProjectionValid = true;
		const auto Restore = [&Edits, &SceneValue, &Workspace](std::string_view Snapshot) -> std::expected<void, std::string>
		{ return Edits.Restore(Snapshot, SceneValue, Workspace); };
		const auto Began = Interaction.Begin(SceneValue, Workspace, Edits, BeginInput, Restore);
		PF_CHECK(Tests, Began && *Began);
		if (!Began || !*Began) return;
		EditorViewportGizmoUpdateInput UpdateInput;
		UpdateInput.MousePosition = MovedMouse;
		UpdateInput.ImageRect = ImageRect;
		UpdateInput.ViewProjection = ViewProjection;
		UpdateInput.ToolValues.LocalSpace = true;
		PF_CHECK(Tests, Interaction.Update(&SceneValue, Workspace, Edits, UpdateInput, Restore));

		const auto UpdatedParent = SceneValue.FindEntity(Parent->GetUUID());
		const auto UpdatedChild = SceneValue.FindEntity(Child->GetUUID());
		PF_CHECK(Tests, UpdatedParent && UpdatedChild);
		if (!UpdatedParent || !UpdatedChild) return;
		const auto ParentTransform = UpdatedParent->GetTransform();
		const auto ChildTransform = UpdatedChild->GetTransform();
		PF_CHECK(Tests, ParentTransform && ChildTransform && SameTransform(*ChildTransform, ChildInitial));
		if (!ParentTransform || !ChildTransform) return;
		if (Operation == TransformGizmoOperation::Translate)
		{
			PF_CHECK(Tests, glm::length(ParentTransform->Translation - ParentInitial.Translation) > 1.0e-3f);
			PF_CHECK(Tests, glm::length(ParentTransform->Scale - ParentInitial.Scale) < 1.0e-5f);
			PF_CHECK(Tests, 1.0f - std::abs(glm::dot(ParentTransform->Rotation, ParentInitial.Rotation)) < 1.0e-5f);
		}
		else if (Operation == TransformGizmoOperation::Rotate)
		{
			PF_CHECK(Tests, glm::length(ParentTransform->Translation - ParentInitial.Translation) < 1.0e-5f);
			PF_CHECK(Tests, glm::length(ParentTransform->Scale - ParentInitial.Scale) < 1.0e-5f);
			PF_CHECK(Tests, 1.0f - std::abs(glm::dot(ParentTransform->Rotation, ParentInitial.Rotation)) > 1.0e-3f);
		}
		else
		{
			PF_CHECK(Tests, glm::length(ParentTransform->Translation - ParentInitial.Translation) < 1.0e-5f);
			PF_CHECK(Tests, 1.0f - std::abs(glm::dot(ParentTransform->Rotation, ParentInitial.Rotation)) < 1.0e-5f);
			PF_CHECK(Tests, std::abs(ParentTransform->Scale.x - ParentInitial.Scale.x) > 1.0e-3f);
			PF_CHECK(Tests, glm::length(glm::vec2(ParentTransform->Scale.y, ParentTransform->Scale.z) -
										glm::vec2(ParentInitial.Scale.y, ParentInitial.Scale.z)) < 1.0e-5f);
		}
		PF_CHECK(Tests, Edits.IsDirty());
		PF_CHECK(Tests, Interaction.Complete(&SceneValue, Workspace, Edits, Restore));
		const auto Undone = Edits.Undo(&SceneValue, Workspace, false, Restore);
		const auto RestoredParent = SceneValue.FindEntity(Parent->GetUUID());
		const auto RestoredChild = SceneValue.FindEntity(Child->GetUUID());
		PF_CHECK(Tests, Undone && *Undone && RestoredParent && RestoredChild);
		if (RestoredParent && RestoredChild)
		{
			PF_CHECK(Tests, SameTransform(*RestoredParent->GetTransform(), ParentInitial));
			PF_CHECK(Tests, SameTransform(*RestoredChild->GetTransform(), ChildInitial));
		}
		PF_CHECK(Tests, !Edits.IsDirty() && !Edits.IsActive());
	}

	ViewportGizmoGeometry MakeTestGeometry()
	{
		ViewportGizmoGeometry Geometry;
		Geometry.IsValid = true;
		Geometry.Axes[0] = { .WorldDirection = { 1.0f, 0.0f, 0.0f }, .WorldUnitsPerLocalUnit = 1.0f, .IsValid = true };
		Geometry.Axes[1] = { .WorldDirection = { 0.0f, 1.0f, 0.0f }, .WorldUnitsPerLocalUnit = 1.0f, .IsValid = true };
		Geometry.Axes[2] = { .WorldDirection = { 0.0f, 0.0f, 1.0f }, .WorldUnitsPerLocalUnit = 1.0f, .IsValid = true };
		return Geometry;
	}

	void RunWorldOperationHistoryCase(
		TestRunner& Tests, TransformGizmoOperation Operation, uint32_t Axis, glm::vec2 StartMouse, glm::vec2 MovedMouse)
	{
		Scene SceneValue;
		auto Entity = SceneValue.CreateEntity("World-space target");
		auto OtherEntity = SceneValue.CreateEntity("Second world-space target");
		PF_CHECK(Tests, Entity.has_value() && OtherEntity.has_value());
		if (!Entity || !OtherEntity) return;
		TransformComponent Initial = *Entity->GetTransform();
		Initial.Translation.x = 1.0f;
		PF_CHECK(Tests, Entity->SetTransform(Initial));
		TransformComponent OtherInitial = *OtherEntity->GetTransform();
		OtherInitial.Translation.y = 1.0f;
		PF_CHECK(Tests, OtherEntity->SetTransform(OtherInitial));

		EditorSceneWorkspaceState Workspace;
		Workspace.Selection().Set({ Entity->GetUUID(), OtherEntity->GetUUID() }, Entity->GetUUID());
		EditorSceneEditController Edits;
		Edits.Reset(&SceneValue);
		EditorViewportGizmoInteraction Interaction;
		const glm::mat4 View = glm::lookAtRH(glm::vec3(0.0f, 0.0f, 5.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
		const glm::mat4 Projection = glm::perspectiveRH_ZO(glm::radians(45.0f), 1.0f, 0.1f, 100.0f);
		const glm::mat4 ViewProjection = Projection * View;
		const ViewportImageRect ImageRect{ { 0.0f, 0.0f }, { 100.0f, 100.0f } };
		EditorViewportGizmoBeginInput BeginInput;
		BeginInput.Handle = { Operation, Axis };
		BeginInput.Geometry = MakeTestGeometry();
		BeginInput.MousePosition = StartMouse;
		BeginInput.ImageRect = ImageRect;
		BeginInput.ViewProjection = ViewProjection;
		BeginInput.CameraPosition = { 0.0f, 0.0f, 5.0f };
		BeginInput.ViewProjectionValid = true;
		const auto Restore = [&Edits, &SceneValue, &Workspace](std::string_view Snapshot) -> std::expected<void, std::string>
		{ return Edits.Restore(Snapshot, SceneValue, Workspace); };

		const auto Began = Interaction.Begin(SceneValue, Workspace, Edits, BeginInput, Restore);
		PF_CHECK(Tests, Began && *Began);
		if (!Began || !*Began) return;
		EditorViewportGizmoUpdateInput UpdateInput;
		UpdateInput.MousePosition = MovedMouse;
		UpdateInput.ImageRect = ImageRect;
		UpdateInput.ViewProjection = ViewProjection;
		UpdateInput.ToolValues.LocalSpace = false;
		PF_CHECK(Tests, Interaction.Update(&SceneValue, Workspace, Edits, UpdateInput, Restore));
		const auto UpdatedEntity = SceneValue.FindEntity(Entity->GetUUID());
		const auto UpdatedTransform = UpdatedEntity ? UpdatedEntity->GetTransform()
													: std::expected<TransformComponent, SceneError>(std::unexpected(
														  SceneError{ SceneErrorCode::InvalidEntity, "test target disappeared" }));
		const auto UpdatedOtherEntity = SceneValue.FindEntity(OtherEntity->GetUUID());
		const auto UpdatedOtherTransform =
			UpdatedOtherEntity ? UpdatedOtherEntity->GetTransform()
							   : std::expected<TransformComponent, SceneError>(
									 std::unexpected(SceneError{ SceneErrorCode::InvalidEntity, "second test target disappeared" }));
		PF_CHECK(Tests, UpdatedTransform && !SameTransform(*UpdatedTransform, Initial));
		PF_CHECK(Tests, UpdatedOtherTransform && !SameTransform(*UpdatedOtherTransform, OtherInitial));
		PF_CHECK(Tests, Interaction.Complete(&SceneValue, Workspace, Edits, Restore));
		PF_CHECK(Tests, Edits.CanUndo() && Edits.IsDirty());
		const auto Undone = Edits.Undo(&SceneValue, Workspace, false, Restore);
		const auto RestoredEntity = SceneValue.FindEntity(Entity->GetUUID());
		const auto RestoredOtherEntity = SceneValue.FindEntity(OtherEntity->GetUUID());
		const auto RestoredTransform = RestoredEntity ? RestoredEntity->GetTransform()
													  : std::expected<TransformComponent, SceneError>(std::unexpected(
															SceneError{ SceneErrorCode::InvalidEntity, "test target disappeared" }));
		const auto RestoredOtherTransform =
			RestoredOtherEntity ? RestoredOtherEntity->GetTransform()
								: std::expected<TransformComponent, SceneError>(
									  std::unexpected(SceneError{ SceneErrorCode::InvalidEntity, "second test target disappeared" }));
		PF_CHECK(Tests, Undone && *Undone && RestoredTransform && SameTransform(*RestoredTransform, Initial));
		PF_CHECK(Tests, RestoredOtherTransform && SameTransform(*RestoredOtherTransform, OtherInitial) && !Edits.IsDirty());
	}

	void RunHierarchyAndFailureCases(TestRunner& Tests)
	{
		using namespace PulseForge;
		using namespace PulseForgeEditor;
		Scene SceneValue;
		auto Parent = SceneValue.CreateEntity("Rotated nonuniform parent");
		auto Child = SceneValue.CreateEntity("Selected child");
		auto OtherRoot = SceneValue.CreateEntity("Second selected root");
		PF_CHECK(Tests, Parent && Child && OtherRoot);
		if (!Parent || !Child || !OtherRoot) return;
		TransformComponent ParentTransform = *Parent->GetTransform();
		ParentTransform.Rotation = glm::angleAxis(glm::radians(45.0f), glm::vec3(0.0f, 0.0f, 1.0f));
		ParentTransform.Scale = { 2.0f, 1.0f, 1.0f };
		PF_CHECK(Tests, Parent->SetTransform(ParentTransform));
		PF_CHECK(Tests, Child->SetParent(*Parent));
		TransformComponent ChildTransform = *Child->GetTransform();
		ChildTransform.Translation = { 0.0f, 1.0f, 0.0f };
		PF_CHECK(Tests, Child->SetTransform(ChildTransform));
		TransformComponent OtherTransform = *OtherRoot->GetTransform();
		OtherTransform.Translation = { 2.0f, 0.0f, 0.0f };
		PF_CHECK(Tests, OtherRoot->SetTransform(OtherTransform));
		const auto WorldPosition = [](const Scene& SceneValue, const UUID& Identifier)
		{
			const auto Target = SceneValue.FindEntity(Identifier);
			if (!Target) return glm::vec3(std::numeric_limits<float>::quiet_NaN());
			const auto World = Target->GetWorldMatrix();
			return World ? glm::vec3((*World)[3]) : glm::vec3(std::numeric_limits<float>::quiet_NaN());
		};
		const glm::vec3 ParentBefore = WorldPosition(SceneValue, Parent->GetUUID());
		const glm::vec3 ChildBefore = WorldPosition(SceneValue, Child->GetUUID());
		const glm::vec3 OtherBefore = WorldPosition(SceneValue, OtherRoot->GetUUID());

		EditorSceneWorkspaceState Workspace;
		Workspace.Selection().Set({ Parent->GetUUID(), Child->GetUUID(), OtherRoot->GetUUID() }, Parent->GetUUID());
		EditorSceneEditController Edits;
		Edits.Reset(&SceneValue);
		EditorViewportGizmoInteraction Interaction;
		const glm::mat4 View = glm::lookAtRH(glm::vec3(0.0f, 0.0f, 5.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
		const glm::mat4 Projection = glm::perspectiveRH_ZO(glm::radians(45.0f), 1.0f, 0.1f, 100.0f);
		const glm::mat4 ViewProjection = Projection * View;
		const ViewportImageRect ImageRect{ { 0.0f, 0.0f }, { 100.0f, 100.0f } };
		EditorViewportGizmoBeginInput BeginInput;
		BeginInput.Handle = { TransformGizmoOperation::Translate, 0 };
		BeginInput.Geometry = MakeTestGeometry();
		BeginInput.MousePosition = { 50.0f, 50.0f };
		BeginInput.ImageRect = ImageRect;
		BeginInput.ViewProjection = ViewProjection;
		BeginInput.CameraPosition = { 0.0f, 0.0f, 5.0f };
		BeginInput.ViewProjectionValid = true;
		const auto Restore = [&Edits, &SceneValue, &Workspace](std::string_view Snapshot) -> std::expected<void, std::string>
		{ return Edits.Restore(Snapshot, SceneValue, Workspace); };
		const auto Began = Interaction.Begin(SceneValue, Workspace, Edits, BeginInput, Restore);
		PF_CHECK(Tests, Began && *Began);
		if (!Began || !*Began) return;
		EditorViewportGizmoUpdateInput UpdateInput;
		UpdateInput.MousePosition = { 75.0f, 50.0f };
		UpdateInput.ImageRect = ImageRect;
		UpdateInput.ViewProjection = ViewProjection;
		PF_CHECK(Tests, Interaction.Update(&SceneValue, Workspace, Edits, UpdateInput, Restore));
		const glm::vec3 ParentDelta = WorldPosition(SceneValue, Parent->GetUUID()) - ParentBefore;
		PF_CHECK(Tests, ParentDelta.x > 0.5f && std::abs(ParentDelta.y) < 1.0e-4f);
		PF_CHECK(Tests, glm::length((WorldPosition(SceneValue, Child->GetUUID()) - ChildBefore) - ParentDelta) < 1.0e-4f);
		PF_CHECK(Tests, glm::length((WorldPosition(SceneValue, OtherRoot->GetUUID()) - OtherBefore) - ParentDelta) < 1.0e-4f);
		PF_CHECK(Tests, SameTransform(*Child->GetTransform(), ChildTransform));
		PF_CHECK(Tests, Interaction.Complete(&SceneValue, Workspace, Edits, Restore));
		const auto Undone = Edits.Undo(&SceneValue, Workspace, false, Restore);
		PF_CHECK(Tests, Undone && *Undone && !Edits.IsDirty());
		PF_CHECK(Tests, glm::length(WorldPosition(SceneValue, Parent->GetUUID()) - ParentBefore) < 1.0e-4f);
		PF_CHECK(Tests, glm::length(WorldPosition(SceneValue, Child->GetUUID()) - ChildBefore) < 1.0e-4f);
		PF_CHECK(Tests, glm::length(WorldPosition(SceneValue, OtherRoot->GetUUID()) - OtherBefore) < 1.0e-4f);

		Scene SingularScene;
		auto SingularParent = SingularScene.CreateEntity("Singular parent");
		auto SingularChild = SingularScene.CreateEntity("Singular child");
		PF_CHECK(Tests, SingularParent && SingularChild);
		if (!SingularParent || !SingularChild) return;
		TransformComponent SingularTransform = *SingularParent->GetTransform();
		SingularTransform.Scale.x = 0.0f;
		PF_CHECK(Tests, SingularParent->SetTransform(SingularTransform));
		PF_CHECK(Tests, SingularChild->SetParent(*SingularParent));
		EditorSceneWorkspaceState SingularWorkspace;
		SingularWorkspace.Selection().SelectSingle(SingularChild->GetUUID());
		EditorSceneEditController SingularEdits;
		SingularEdits.Reset(&SingularScene);
		EditorViewportGizmoInteraction SingularInteraction;
		const auto SingularRestore = [&SingularEdits, &SingularScene, &SingularWorkspace](
										 std::string_view Snapshot) -> std::expected<void, std::string>
		{ return SingularEdits.Restore(Snapshot, SingularScene, SingularWorkspace); };
		const auto SingularBegin = SingularInteraction.Begin(SingularScene, SingularWorkspace, SingularEdits, BeginInput, SingularRestore);
		PF_CHECK(Tests, SingularBegin && !*SingularBegin && !SingularEdits.IsActive());

		Scene FailureScene;
		auto FirstRoot = FailureScene.CreateEntity("First transform target");
		auto ShearedParent = FailureScene.CreateEntity("Rotated nonuniform parent");
		auto ShearedChild = FailureScene.CreateEntity("Shear target");
		PF_CHECK(Tests, FirstRoot && ShearedParent && ShearedChild);
		if (!FirstRoot || !ShearedParent || !ShearedChild) return;
		TransformComponent FirstInitial = *FirstRoot->GetTransform();
		FirstInitial.Translation.x = 1.0f;
		PF_CHECK(Tests, FirstRoot->SetTransform(FirstInitial));
		TransformComponent ShearedParentInitial = *ShearedParent->GetTransform();
		ShearedParentInitial.Rotation = glm::angleAxis(glm::radians(45.0f), glm::vec3(0.0f, 0.0f, 1.0f));
		ShearedParentInitial.Scale = { 2.0f, 1.0f, 1.0f };
		PF_CHECK(Tests, ShearedParent->SetTransform(ShearedParentInitial));
		PF_CHECK(Tests, ShearedChild->SetParent(*ShearedParent));
		const glm::mat4 ChildWorldBefore = *ShearedChild->GetWorldMatrix();
		EditorSceneWorkspaceState FailureWorkspace;
		FailureWorkspace.Selection().Set({ FirstRoot->GetUUID(), ShearedChild->GetUUID() }, FirstRoot->GetUUID());
		EditorSceneEditController FailureEdits;
		FailureEdits.Reset(&FailureScene);
		EditorViewportGizmoInteraction FailureInteraction;
		const auto FailureRestore = [&FailureEdits, &FailureScene, &FailureWorkspace](
										std::string_view Snapshot) -> std::expected<void, std::string>
		{ return FailureEdits.Restore(Snapshot, FailureScene, FailureWorkspace); };
		BeginInput.Handle = { TransformGizmoOperation::Scale, 0 };
		BeginInput.MousePosition = { 50.0f, 50.0f };
		const auto FailureBegin = FailureInteraction.Begin(FailureScene, FailureWorkspace, FailureEdits, BeginInput, FailureRestore);
		PF_CHECK(Tests, FailureBegin && *FailureBegin);
		if (!FailureBegin || !*FailureBegin) return;
		UpdateInput.ToolValues.LocalSpace = false;
		const auto FailedUpdate = FailureInteraction.Update(&FailureScene, FailureWorkspace, FailureEdits, UpdateInput, FailureRestore);
		PF_CHECK(Tests, !FailedUpdate && FailedUpdate.error().find("unsupported shear") != std::string::npos);
		PF_CHECK(Tests, !FailureInteraction.IsActive() && !FailureEdits.IsActive() && !FailureEdits.IsDirty() && !FailureEdits.CanUndo());
		const auto RestoredFirst = FailureScene.FindEntity(FirstRoot->GetUUID());
		const auto RestoredChild = FailureScene.FindEntity(ShearedChild->GetUUID());
		PF_CHECK(Tests, RestoredFirst && RestoredChild);
		if (RestoredFirst && RestoredChild)
		{
			PF_CHECK(Tests, SameTransform(*RestoredFirst->GetTransform(), FirstInitial));
			const auto RestoredChildWorld = RestoredChild->GetWorldMatrix();
			PF_CHECK(Tests, RestoredChildWorld && glm::length(glm::vec3((*RestoredChildWorld)[3] - ChildWorldBefore[3])) < 1.0e-4f);
		}
	}
} // namespace

void RunEditorViewportGizmoInteractionTests(TestRunner& Tests)
{
	using namespace PulseForge;
	using namespace PulseForgeEditor;

	Scene SceneValue;
	auto Entity = SceneValue.CreateEntity("Gizmo target");
	PF_CHECK(Tests, Entity.has_value());
	if (!Entity) return;

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
	const auto Restore = [&Edits, &SceneValue, &Workspace](std::string_view Snapshot) -> std::expected<void, std::string>
	{ return Edits.Restore(Snapshot, SceneValue, Workspace); };
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
	if (!Began || !*Began) return;
	PF_CHECK(Tests, Interaction.Update(&SceneValue, Workspace, Edits, UpdateInput(75.0f, false), Restore));
	const auto Moved = Entity->GetTransform();
	PF_CHECK(Tests, Moved && Moved->Translation.x > 0.1f && Edits.IsDirty());
	PF_CHECK(Tests, Interaction.Complete(&SceneValue, Workspace, Edits, Restore));
	PF_CHECK(Tests, !Interaction.IsActive() && Edits.CanUndo());
	const auto Undone = Edits.Undo(&SceneValue, Workspace, false, Restore);
	PF_CHECK(Tests, Undone && *Undone);
	const auto Restored = SceneValue.FindEntity(Entity->GetUUID());
	PF_CHECK(Tests, Restored.has_value());
	if (!Restored) return;
	const auto RestoredTransform = Restored->GetTransform();
	PF_CHECK(Tests, RestoredTransform && glm::length(RestoredTransform->Translation) < 1.0e-5f);

	const auto BeganCancel = Interaction.Begin(SceneValue, Workspace, Edits, BeginInput(false), Restore);
	PF_CHECK(Tests, BeganCancel && *BeganCancel);
	if (!BeganCancel || !*BeganCancel) return;
	PF_CHECK(Tests, Interaction.Update(&SceneValue, Workspace, Edits, UpdateInput(72.0f, false), Restore));
	PF_CHECK(Tests, Interaction.Cancel(&SceneValue, Workspace, Edits, Restore));
	PF_CHECK(Tests, !Interaction.IsActive() && !Edits.IsActive());
	const auto Cancelled = SceneValue.FindEntity(Entity->GetUUID());
	PF_CHECK(Tests, Cancelled.has_value());
	if (!Cancelled) return;
	const auto CancelledTransform = Cancelled->GetTransform();
	PF_CHECK(Tests, CancelledTransform && glm::length(CancelledTransform->Translation) < 1.0e-5f);

	const auto BeganFailedCancel = Interaction.Begin(SceneValue, Workspace, Edits, BeginInput(false), Restore);
	PF_CHECK(Tests, BeganFailedCancel && *BeganFailedCancel);
	if (!BeganFailedCancel || !*BeganFailedCancel) return;
	PF_CHECK(Tests, Interaction.Update(&SceneValue, Workspace, Edits, UpdateInput(75.0f, false), Restore));
	const auto FailingRestore = [](std::string_view) -> std::expected<void, std::string>
	{ return std::unexpected("Injected scene restore failure"); };
	const auto FailedCancel = Interaction.Cancel(&SceneValue, Workspace, Edits, FailingRestore);
	PF_CHECK(Tests, !FailedCancel && Interaction.IsActive() && Edits.IsActive());
	const auto PendingRollback = SceneValue.FindEntity(Entity->GetUUID());
	PF_CHECK(Tests, PendingRollback.has_value());
	if (!PendingRollback) return;
	const auto PendingTransform = PendingRollback->GetTransform();
	PF_CHECK(Tests, PendingTransform && PendingTransform->Translation.x > 0.1f);
	PF_CHECK(Tests, Interaction.Cancel(&SceneValue, Workspace, Edits, Restore));
	PF_CHECK(Tests, !Interaction.IsActive() && !Edits.IsActive());
	const auto RetriedRollback = SceneValue.FindEntity(Entity->GetUUID());
	PF_CHECK(Tests, RetriedRollback.has_value());
	if (!RetriedRollback) return;
	const auto RetriedTransform = RetriedRollback->GetTransform();
	PF_CHECK(Tests, RetriedTransform && glm::length(RetriedTransform->Translation) < 1.0e-5f);
	Edits.MarkSaved(&SceneValue);

	const auto BeganCompletionRetry = Interaction.Begin(SceneValue, Workspace, Edits, BeginInput(false), Restore);
	PF_CHECK(Tests, BeganCompletionRetry && *BeganCompletionRetry);
	if (!BeganCompletionRetry || !*BeganCompletionRetry) return;
	PF_CHECK(Tests, Interaction.Update(&SceneValue, Workspace, Edits, UpdateInput(72.0f, false), Restore));
	const auto FailedCompletion = Interaction.Complete(nullptr, Workspace, Edits, Restore);
	PF_CHECK(Tests, !FailedCompletion && Interaction.IsActive() && Edits.IsActive() && Edits.IsDirty());
	const auto RetainedCompletionTarget = SceneValue.FindEntity(Entity->GetUUID());
	PF_CHECK(Tests, RetainedCompletionTarget.has_value());
	if (!RetainedCompletionTarget) return;
	const auto RetainedCompletionTransform = RetainedCompletionTarget->GetTransform();
	PF_CHECK(Tests, RetainedCompletionTransform && RetainedCompletionTransform->Translation.x > 0.1f);
	PF_CHECK(Tests, Interaction.Complete(&SceneValue, Workspace, Edits, Restore));
	PF_CHECK(Tests, !Interaction.IsActive() && !Edits.IsActive() && Edits.CanUndo() && Edits.IsDirty());
	const auto CompletionUndo = Edits.Undo(&SceneValue, Workspace, false, Restore);
	const auto CompletionUndoTarget = SceneValue.FindEntity(Entity->GetUUID());
	PF_CHECK(Tests, CompletionUndo && *CompletionUndo && CompletionUndoTarget.has_value());
	if (CompletionUndoTarget)
	{
		const auto CompletionUndoTransform = CompletionUndoTarget->GetTransform();
		PF_CHECK(Tests, CompletionUndoTransform && glm::length(CompletionUndoTransform->Translation) < 1.0e-5f && !Edits.IsDirty());
	}

	auto OtherEntity = SceneValue.CreateEntity("Other selection");
	PF_CHECK(Tests, OtherEntity.has_value());
	if (!OtherEntity) return;
	const auto BeganSelectionChange = Interaction.Begin(SceneValue, Workspace, Edits, BeginInput(false), Restore);
	PF_CHECK(Tests, BeganSelectionChange && *BeganSelectionChange);
	if (!BeganSelectionChange || !*BeganSelectionChange) return;
	PF_CHECK(Tests, Interaction.Update(&SceneValue, Workspace, Edits, UpdateInput(72.0f, false), Restore));
	Workspace.Selection().SelectSingle(OtherEntity->GetUUID());
	PF_CHECK(Tests, Interaction.Update(&SceneValue, Workspace, Edits, UpdateInput(75.0f, false), Restore));
	const auto SelectionChangeRestored = SceneValue.FindEntity(Entity->GetUUID());
	PF_CHECK(Tests, SelectionChangeRestored.has_value());
	if (!SelectionChangeRestored) return;
	const auto SelectionChangeTransform = SelectionChangeRestored->GetTransform();
	PF_CHECK(Tests, !Interaction.IsActive() && !Edits.IsActive() && SelectionChangeTransform &&
						glm::length(SelectionChangeTransform->Translation) < 1.0e-5f);
	Edits.MarkSaved(&SceneValue);

	Workspace.Selection().SelectSingle(Entity->GetUUID());
	Scene ReplacementScene;
	auto ReplacementEntity = ReplacementScene.CreateEntity("Replacement scene sentinel");
	PF_CHECK(Tests, ReplacementEntity.has_value());
	if (!ReplacementEntity) return;
	TransformComponent ReplacementInitial = *ReplacementEntity->GetTransform();
	ReplacementInitial.Translation = { 3.0f, 2.0f, 1.0f };
	PF_CHECK(Tests, ReplacementEntity->SetTransform(ReplacementInitial));
	EditorSceneWorkspaceState ReplacementWorkspace;
	Scene* ActiveScene = &SceneValue;
	EditorSceneWorkspaceState* ActiveWorkspace = &Workspace;
	uint32_t RestoreCalls = 0;
	const auto RestoreActiveScene = [&](std::string_view Snapshot) -> std::expected<void, std::string>
	{
		++RestoreCalls;
		return Edits.Restore(Snapshot, *ActiveScene, *ActiveWorkspace);
	};
	const auto BeganSceneChange = Interaction.Begin(SceneValue, Workspace, Edits, BeginInput(false), RestoreActiveScene);
	PF_CHECK(Tests, BeganSceneChange && *BeganSceneChange);
	if (!BeganSceneChange || !*BeganSceneChange) return;
	PF_CHECK(Tests, Interaction.Update(&SceneValue, Workspace, Edits, UpdateInput(75.0f, false), RestoreActiveScene));
	const uint32_t RestoreCallsBeforeChange = RestoreCalls;
	ActiveScene = &ReplacementScene;
	ActiveWorkspace = &ReplacementWorkspace;
	const auto RejectedSceneUpdate =
		Interaction.Update(&ReplacementScene, ReplacementWorkspace, Edits, UpdateInput(50.0f, false), RestoreActiveScene);
	PF_CHECK(Tests, !RejectedSceneUpdate && Interaction.IsActive() && Edits.IsActive());
	PF_CHECK(Tests, RestoreCalls == RestoreCallsBeforeChange);
	const auto ReplacementAfterRejectedUpdate = ReplacementScene.FindEntity(ReplacementEntity->GetUUID());
	PF_CHECK(Tests, ReplacementAfterRejectedUpdate && SameTransform(*ReplacementAfterRejectedUpdate->GetTransform(), ReplacementInitial));
	const auto RejectedSceneCompletion = Interaction.Complete(&ReplacementScene, ReplacementWorkspace, Edits, RestoreActiveScene);
	const auto RejectedSceneCancellation = Interaction.Cancel(&ReplacementScene, ReplacementWorkspace, Edits, RestoreActiveScene);
	PF_CHECK(Tests, !RejectedSceneCompletion && !RejectedSceneCancellation && Interaction.IsActive() && Edits.IsActive());
	PF_CHECK(Tests, RestoreCalls == RestoreCallsBeforeChange);
	ActiveScene = &SceneValue;
	ActiveWorkspace = &Workspace;
	PF_CHECK(Tests, Interaction.Cancel(&SceneValue, Workspace, Edits, RestoreActiveScene));
	PF_CHECK(Tests, !Interaction.IsActive() && !Edits.IsActive() && !Edits.IsDirty());
	const auto OriginalAfterSceneChange = SceneValue.FindEntity(Entity->GetUUID());
	const auto ReplacementAfterRollback = ReplacementScene.FindEntity(ReplacementEntity->GetUUID());
	PF_CHECK(Tests, OriginalAfterSceneChange && glm::length(OriginalAfterSceneChange->GetTransform()->Translation) < 1.0e-5f);
	PF_CHECK(Tests, ReplacementAfterRollback && SameTransform(*ReplacementAfterRollback->GetTransform(), ReplacementInitial));

	const auto BeganDuplicate = Interaction.Begin(SceneValue, Workspace, Edits, BeginInput(true), Restore);
	PF_CHECK(Tests, BeganDuplicate && *BeganDuplicate);
	if (!BeganDuplicate || !*BeganDuplicate) return;
	PF_CHECK(Tests, Interaction.Update(&SceneValue, Workspace, Edits, UpdateInput(75.0f, true), Restore));
	const auto Duplicate = Workspace.Selection().GetPrimary();
	PF_CHECK(Tests, Duplicate && *Duplicate != Entity->GetUUID() && SceneValue.FindEntity(*Duplicate).has_value());
	if (!Duplicate) return;
	PF_CHECK(Tests, Interaction.Complete(&SceneValue, Workspace, Edits, Restore));
	const auto DuplicateUndo = Edits.Undo(&SceneValue, Workspace, false, Restore);
	PF_CHECK(Tests, DuplicateUndo && *DuplicateUndo && !SceneValue.FindEntity(*Duplicate).has_value());
	PF_CHECK(Tests, Workspace.Selection().GetPrimary() == Entity->GetUUID());
	const size_t EntityCountBeforeCancelledDuplicate = SceneValue.GetEntityCount();
	const bool HadRedoBeforeCancelledDuplicate = Edits.CanRedo();
	const auto BeganCancelledDuplicate = Interaction.Begin(SceneValue, Workspace, Edits, BeginInput(true), Restore);
	PF_CHECK(Tests, BeganCancelledDuplicate && *BeganCancelledDuplicate);
	if (BeganCancelledDuplicate && *BeganCancelledDuplicate)
	{
		PF_CHECK(Tests, Interaction.Update(&SceneValue, Workspace, Edits, UpdateInput(75.0f, true), Restore));
		const auto TemporaryDuplicate = Workspace.Selection().GetPrimary();
		PF_CHECK(Tests, TemporaryDuplicate && *TemporaryDuplicate != Entity->GetUUID());
		PF_CHECK(Tests, Interaction.Cancel(&SceneValue, Workspace, Edits, Restore));
		PF_CHECK(Tests, SceneValue.GetEntityCount() == EntityCountBeforeCancelledDuplicate);
		PF_CHECK(Tests, Workspace.Selection().GetPrimary() == Entity->GetUUID());
		PF_CHECK(Tests, !Interaction.IsActive());
		PF_CHECK(Tests, !Edits.IsActive());
		PF_CHECK(Tests, !Edits.IsDirty());
		PF_CHECK(Tests, !Edits.CanUndo());
		PF_CHECK(Tests, Edits.CanRedo() == HadRedoBeforeCancelledDuplicate);
	}

	RunLocalReturnToOriginCase(Tests, TransformGizmoOperation::Translate, 0, { 50.0f, 50.0f }, { 75.0f, 50.0f }, { 61.0f, 50.0f });
	RunLocalReturnToOriginCase(Tests, TransformGizmoOperation::Rotate, 2, { 75.0f, 50.0f }, { 72.0f, 42.0f }, { 74.0f, 48.0f });
	RunLocalReturnToOriginCase(Tests, TransformGizmoOperation::Scale, 0, { 50.0f, 50.0f }, { 75.0f, 50.0f }, { 51.0f, 50.0f });
	RunWorldOperationHistoryCase(Tests, TransformGizmoOperation::Rotate, 2, { 75.0f, 50.0f }, { 50.0f, 75.0f });
	RunWorldOperationHistoryCase(Tests, TransformGizmoOperation::Scale, 0, { 50.0f, 50.0f }, { 75.0f, 50.0f });
	RunParentChildLocalTransformCase(Tests, TransformGizmoOperation::Translate);
	RunParentChildLocalTransformCase(Tests, TransformGizmoOperation::Rotate);
	RunParentChildLocalTransformCase(Tests, TransformGizmoOperation::Scale);
	RunHierarchyAndFailureCases(Tests);
}
