#include "Editor/EditorViewportGizmoInteraction.h"

#include "Editor/EditorSceneEditController.h"
#include "Editor/EditorSceneHierarchyOperations.h"
#include "Editor/EditorSceneWorkspaceState.h"
#include "Editor/ViewportMath.h"
#include "Scene/Scene.h"

#include <cmath>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/matrix_decompose.hpp>

namespace PulseForgeEditor
{
	std::expected<bool, std::string> EditorViewportGizmoInteraction::Begin(PulseForge::Scene& SceneValue,
		EditorSceneWorkspaceState& Workspace, EditorSceneEditController& EditController, const EditorViewportGizmoBeginInput& Input,
		const RestoreCallback& RestoreState)
	{
		if (m_Drag || !Input.Geometry.IsValid || Input.RuntimeActive || Input.CameraNavigationActive || !Input.ViewProjectionValid ||
			Input.Handle.Axis >= Input.Geometry.Axes.size() || !Input.Geometry.Axes[Input.Handle.Axis].IsValid)
			return false;

		const auto SelectedIdentifier = Workspace.Selection().GetPrimary();
		if (!SelectedIdentifier) return false;
		const auto Entity = SceneValue.FindEntity(*SelectedIdentifier);
		if (!Entity) return false;
		const auto Transform = Entity->GetTransform();
		const auto Ray = MakeViewportRay(Input.MousePosition, Input.ImageRect, Input.ViewProjection);
		if (!Transform || !Ray) return false;

		const glm::vec3 WorldAxis = Input.Geometry.Axes[Input.Handle.Axis].WorldDirection;
		glm::vec3 PlaneNormal = WorldAxis;
		if (Input.Handle.Operation != TransformGizmoOperation::Rotate)
		{
			const glm::vec3 ToCamera = Input.CameraPosition - Input.Geometry.PivotWorld;
			PlaneNormal = ToCamera - WorldAxis * glm::dot(ToCamera, WorldAxis);
		}
		const float PlaneNormalLength = glm::length(PlaneNormal);
		if (!std::isfinite(PlaneNormalLength) || PlaneNormalLength < 1.0e-5f) return false;
		PlaneNormal /= PlaneNormalLength;
		const auto StartPoint = IntersectRayPlane(*Ray, Input.Geometry.PivotWorld, PlaneNormal);
		if (!StartPoint) return false;

		auto Roots = EditorSceneHierarchyOperations::GetTopLevelSelection(SceneValue, Workspace.Selection().GetEntities());
		if (!Roots || Roots->empty()) return false;
		std::vector<PulseForge::UUID> RootIdentifiers;
		RootIdentifiers.reserve(Roots->size());
		for (const PulseForge::Entity& Root : *Roots)
			RootIdentifiers.push_back(Root.GetUUID());
		auto Targets = CaptureTargets(SceneValue, RootIdentifiers);
		if (!Targets) return false;

		const std::string Description = Input.Handle.Operation == TransformGizmoOperation::Translate ? "Move Selection"
										: Input.Handle.Operation == TransformGizmoOperation::Rotate	 ? "Rotate Selection"
																									 : "Scale Selection";
		if (auto Begun = EditController.Begin(&SceneValue, Workspace, Input.RuntimeActive, Description, RestoreState); !Begun)
			return std::unexpected(Begun.error());

		DragState Drag;
		Drag.Entity = *SelectedIdentifier;
		Drag.SceneIdentity = &SceneValue;
		Drag.InitialTransform = *Transform;
		Drag.Targets = std::move(*Targets);
		Drag.SourceRoots = std::move(RootIdentifiers);
		Drag.Operation = Input.Handle.Operation;
		Drag.Axis = Input.Handle.Axis;
		Drag.PivotWorld = Input.Geometry.PivotWorld;
		Drag.WorldAxis = WorldAxis;
		Drag.PlaneNormal = PlaneNormal;
		Drag.StartPoint = *StartPoint;
		Drag.StartMouse = Input.MousePosition;
		Drag.WorldUnitsPerLocalUnit = Input.Geometry.Axes[Input.Handle.Axis].WorldUnitsPerLocalUnit;
		Drag.DuplicateOnDrag = Input.DuplicateOnDrag;
		m_Drag = std::move(Drag);
		return true;
	}

	std::expected<void, std::string> EditorViewportGizmoInteraction::Update(PulseForge::Scene* SceneValue,
		EditorSceneWorkspaceState& Workspace, EditorSceneEditController& EditController, const EditorViewportGizmoUpdateInput& Input,
		const RestoreCallback& RestoreState)
	{
		if (!m_Drag) return {};
		const auto SelectedIdentifier = Workspace.Selection().GetPrimary();
		if (!SceneValue || SceneValue != m_Drag->SceneIdentity || Input.RuntimeActive || !SelectedIdentifier ||
			*SelectedIdentifier != m_Drag->Entity)
		{
			if (auto Cancelled = Cancel(SceneValue, Workspace, EditController, RestoreState); !Cancelled)
				return std::unexpected("Scene edit rollback failed: " + Cancelled.error());
			return {};
		}

		DragState& Drag = *m_Drag;
		const auto Ray = MakeViewportRay(Input.MousePosition, Input.ImageRect, Input.ViewProjection, true);
		if (!Ray) return {};
		const auto CurrentPoint = IntersectRayPlane(*Ray, Drag.PivotWorld, Drag.PlaneNormal);
		if (!CurrentPoint) return {};

		float Delta = 0.0f;
		float WorldDelta = 0.0f;
		if (Drag.Operation == TransformGizmoOperation::Rotate)
		{
			const glm::vec3 StartVector = glm::normalize(Drag.StartPoint - Drag.PivotWorld);
			const glm::vec3 CurrentVector = glm::normalize(*CurrentPoint - Drag.PivotWorld);
			Delta = std::atan2(glm::dot(Drag.WorldAxis, glm::cross(StartVector, CurrentVector)), glm::dot(StartVector, CurrentVector));
		}
		else
		{
			WorldDelta = glm::dot(*CurrentPoint - Drag.StartPoint, Drag.WorldAxis);
			if (!std::isfinite(Drag.WorldUnitsPerLocalUnit) || Drag.WorldUnitsPerLocalUnit < 1.0e-7f) return {};
			if (Drag.Operation == TransformGizmoOperation::Translate && (Input.ToolValues.TransformSnapping || Input.ControlDown))
			{
				const auto Snapped = SnapValueToIncrement(WorldDelta, Input.ToolValues.TranslationSnapIncrement);
				if (!Snapped) return {};
				WorldDelta = *Snapped;
			}
			Delta = WorldDelta / Drag.WorldUnitsPerLocalUnit;
		}
		if (!std::isfinite(Delta)) return {};
		if (Input.ToolValues.TransformSnapping || Input.ControlDown)
		{
			const float Increment = Drag.Operation == TransformGizmoOperation::Rotate ? glm::radians(Input.ToolValues.RotationSnapDegrees)
																					  : Input.ToolValues.ScaleSnapIncrement;
			if (Drag.Operation != TransformGizmoOperation::Translate)
			{
				const auto Snapped = SnapValueToIncrement(Delta, Increment);
				if (!Snapped) return {};
				Delta = *Snapped;
			}
		}

		const glm::vec2 MouseDelta = Input.MousePosition - Drag.StartMouse;
		if (Drag.DuplicateOnDrag && !Input.AltDown) Drag.DuplicateOnDrag = false;
		if (Drag.DuplicateOnDrag && !Drag.HasDuplicated && glm::length(MouseDelta) >= 3.0f)
		{
			std::vector<PulseForge::Entity> Sources;
			Sources.reserve(Drag.SourceRoots.size());
			for (const PulseForge::UUID& Identifier : Drag.SourceRoots)
			{
				const auto Source = SceneValue->FindEntity(Identifier);
				if (!Source)
					return CancelAfterFailure(SceneValue, Workspace, EditController, RestoreState,
						"Alt-drag duplication failed because a selected source no longer exists");
				Sources.push_back(*Source);
			}
			auto Duplicated = EditorSceneHierarchyOperations::DuplicateSubtrees(*SceneValue, Sources);
			if (!Duplicated)
				return CancelAfterFailure(
					SceneValue, Workspace, EditController, RestoreState, "Alt-drag duplication failed: " + Duplicated.error().Message);
			Workspace.Selection().Set(*Duplicated);
			auto DuplicatedTargets = CaptureTargets(*SceneValue, *Duplicated);
			if (!DuplicatedTargets)
				return CancelAfterFailure(SceneValue, Workspace, EditController, RestoreState,
					"Could not initialize the duplicated selection for the gizmo drag.");
			Drag.Targets = std::move(*DuplicatedTargets);
			const auto Primary = Workspace.Selection().GetPrimary();
			if (!Primary)
				return CancelAfterFailure(
					SceneValue, Workspace, EditController, RestoreState, "Could not select the duplicated entities for the gizmo drag.");
			Drag.Entity = *Primary;
			Drag.InitialTransform = Drag.Targets.back().LocalTransform;
			Drag.HasDuplicated = true;
			Drag.DuplicateOnDrag = false;
		}

		if (Drag.Targets.empty())
			return CancelAfterFailure(SceneValue, Workspace, EditController, RestoreState, "Viewport transform has no selected targets");

		if (Drag.Targets.size() == 1 && !Drag.HasDuplicated && Input.ToolValues.LocalSpace)
		{
			const auto Entity = SceneValue->FindEntity(Drag.Targets.front().Identifier);
			if (!Entity)
				return CancelAfterFailure(
					SceneValue, Workspace, EditController, RestoreState, "The selected entity was removed during the viewport transform.");
			const auto Updated = ApplyLocalGizmoDelta(Drag.InitialTransform, Drag.Operation, Drag.Axis, Delta);
			if (!Updated) return {};
			const auto Current = Entity->GetTransform();
			if (!Current)
				return CancelAfterFailure(
					SceneValue, Workspace, EditController, RestoreState, "The selected entity no longer has a transform component.");
			if (!TransformsDiffer(*Current, *Updated)) return {};
			if (auto Result = Entity->SetTransform(*Updated); !Result)
				return CancelAfterFailure(SceneValue, Workspace, EditController, RestoreState,
					"Viewport transform manipulation failed: " + Result.error().Message);
			EditController.MarkDirty();
			return {};
		}

		glm::mat4 WorldOperation(1.0f);
		if (Drag.Operation == TransformGizmoOperation::Translate)
			WorldOperation = glm::translate(glm::mat4(1.0f), Drag.WorldAxis * WorldDelta);
		else if (Drag.Operation == TransformGizmoOperation::Rotate)
		{
			const glm::quat Rotation = glm::angleAxis(Delta, glm::normalize(Drag.WorldAxis));
			WorldOperation = glm::translate(glm::mat4(1.0f), Drag.PivotWorld) * glm::mat4_cast(Rotation) *
							 glm::translate(glm::mat4(1.0f), -Drag.PivotWorld);
		}
		else if (Drag.Operation == TransformGizmoOperation::Scale)
		{
			const float Factor = 1.0f + Delta;
			if (!std::isfinite(Factor) || Factor <= 1.0e-4f) return {};
			const glm::vec3 Scale = Drag.Axis == 0	 ? glm::vec3(Factor, 1.0f, 1.0f)
									: Drag.Axis == 1 ? glm::vec3(1.0f, Factor, 1.0f)
													 : glm::vec3(1.0f, 1.0f, Factor);
			WorldOperation = glm::translate(glm::mat4(1.0f), Drag.PivotWorld) * glm::scale(glm::mat4(1.0f), Scale) *
							 glm::translate(glm::mat4(1.0f), -Drag.PivotWorld);
		}

		for (const EntityStart& Target : Drag.Targets)
		{
			const auto Entity = SceneValue->FindEntity(Target.Identifier);
			std::optional<PulseForge::TransformComponent> Updated;
			if (Drag.Operation == TransformGizmoOperation::Scale && Input.ToolValues.LocalSpace)
			{
				const float Factor = 1.0f + Delta;
				PulseForge::TransformComponent Scaled = Target.LocalTransform;
				Scaled.Scale[Drag.Axis] *= Factor;
				const glm::vec3 Position(Target.WorldTransform[3]);
				const float AlongAxis = glm::dot(Position - Drag.PivotWorld, Drag.WorldAxis);
				const glm::vec3 ScaledPosition = Position + Drag.WorldAxis * AlongAxis * (Factor - 1.0f);
				const glm::vec3 LocalPosition(glm::inverse(Target.ParentWorldTransform) * glm::vec4(ScaledPosition, 1.0f));
				Scaled.Translation = LocalPosition;
				if (std::abs(Scaled.Scale[Drag.Axis]) >= 1.0e-4f && Detail::IsFinite(Scaled.Translation) && Detail::IsFinite(Scaled.Scale))
					Updated = Scaled;
			}
			else
			{
				const glm::mat4 LocalMatrix = glm::inverse(Target.ParentWorldTransform) * WorldOperation * Target.WorldTransform;
				Updated = DecomposeLocalTransform(LocalMatrix);
			}
			if (!Entity || !Updated)
				return CancelAfterFailure(SceneValue, Workspace, EditController, RestoreState,
					"The multi-object transform would require an unsupported shear or singular transform.");
			if (auto Result = Entity->SetTransform(*Updated); !Result)
				return CancelAfterFailure(SceneValue, Workspace, EditController, RestoreState,
					"Viewport transform manipulation failed: " + Result.error().Message);
		}
		EditController.MarkDirty();
		return {};
	}

	std::expected<void, std::string> EditorViewportGizmoInteraction::Cancel(PulseForge::Scene* SceneValue,
		EditorSceneWorkspaceState& Workspace, EditorSceneEditController& EditController, const RestoreCallback& RestoreState)
	{
		if (!m_Drag) return {};
		auto Cancelled = EditController.Cancel(SceneValue, Workspace, RestoreState);
		if (!EditController.IsActive()) m_Drag.reset();
		return Cancelled;
	}

	std::expected<void, std::string> EditorViewportGizmoInteraction::Complete(PulseForge::Scene* SceneValue,
		EditorSceneWorkspaceState& Workspace, EditorSceneEditController& EditController, const RestoreCallback& RestoreState)
	{
		if (!m_Drag) return {};
		auto Completed = EditController.Finish(SceneValue, Workspace, RestoreState);
		if (!EditController.IsActive()) m_Drag.reset();
		return Completed;
	}

	std::optional<std::vector<EditorViewportGizmoInteraction::EntityStart>> EditorViewportGizmoInteraction::CaptureTargets(
		const PulseForge::Scene& SceneValue, const std::vector<PulseForge::UUID>& Identifiers)
	{
		if (Identifiers.empty()) return std::nullopt;
		std::vector<EntityStart> Targets;
		Targets.reserve(Identifiers.size());
		for (const PulseForge::UUID& Identifier : Identifiers)
		{
			const auto Entity = SceneValue.FindEntity(Identifier);
			if (!Entity) return std::nullopt;
			const auto Transform = Entity->GetTransform();
			const auto World = Entity->GetWorldMatrix();
			const auto Parent = Entity->GetParent();
			if (!Transform || !World || !Parent || !Detail::IsFinite(*World)) return std::nullopt;
			glm::mat4 ParentWorld(1.0f);
			if (Parent->has_value())
			{
				const auto ParentMatrix = Parent->value().GetWorldMatrix();
				if (!ParentMatrix || !Detail::IsFinite(*ParentMatrix) || std::abs(glm::determinant(*ParentMatrix)) < 1.0e-8f)
					return std::nullopt;
				ParentWorld = *ParentMatrix;
			}
			Targets.push_back({ Identifier, *Transform, *World, ParentWorld });
		}
		return Targets;
	}

	std::optional<PulseForge::TransformComponent> EditorViewportGizmoInteraction::DecomposeLocalTransform(const glm::mat4& LocalMatrix)
	{
		if (!Detail::IsFinite(LocalMatrix)) return std::nullopt;
		glm::vec3 Scale(1.0f);
		glm::quat Rotation(1.0f, 0.0f, 0.0f, 0.0f);
		glm::vec3 Translation(0.0f);
		glm::vec3 Skew(0.0f);
		glm::vec4 Perspective(0.0f, 0.0f, 0.0f, 1.0f);
		if (!glm::decompose(LocalMatrix, Scale, Rotation, Translation, Skew, Perspective)) return std::nullopt;
		const bool PerspectiveFinite =
			std::isfinite(Perspective.x) && std::isfinite(Perspective.y) && std::isfinite(Perspective.z) && std::isfinite(Perspective.w);
		if (!Detail::IsFinite(Scale) || !Detail::IsFinite(Rotation) || !Detail::IsFinite(Translation) || !Detail::IsFinite(Skew) ||
			!PerspectiveFinite || glm::length(Skew) > 1.0e-3f || glm::length(glm::vec3(Perspective)) > 1.0e-3f ||
			std::abs(Perspective.w - 1.0f) > 1.0e-3f || std::abs(Scale.x) < 1.0e-5f || std::abs(Scale.y) < 1.0e-5f ||
			std::abs(Scale.z) < 1.0e-5f)
			return std::nullopt;
		PulseForge::TransformComponent Transform;
		Transform.Translation = Translation;
		Transform.Rotation = glm::normalize(Rotation);
		Transform.Scale = Scale;
		return Transform;
	}

	bool EditorViewportGizmoInteraction::TransformsDiffer(
		const PulseForge::TransformComponent& First, const PulseForge::TransformComponent& Second)
	{
		constexpr float Epsilon = 1.0e-6f;
		if (glm::length(First.Translation - Second.Translation) > Epsilon || glm::length(First.Scale - Second.Scale) > Epsilon) return true;
		const glm::quat FirstRotation = glm::normalize(First.Rotation);
		const glm::quat SecondRotation = glm::normalize(Second.Rotation);
		return 1.0f - std::abs(glm::dot(FirstRotation, SecondRotation)) > Epsilon;
	}

	std::expected<void, std::string> EditorViewportGizmoInteraction::CancelAfterFailure(PulseForge::Scene* SceneValue,
		EditorSceneWorkspaceState& Workspace, EditorSceneEditController& EditController, const RestoreCallback& RestoreState,
		std::string Error)
	{
		auto Rollback = Cancel(SceneValue, Workspace, EditController, RestoreState);
		if (!Rollback) return std::unexpected(Error + "; scene rollback failed: " + Rollback.error());
		return std::unexpected(std::move(Error));
	}
} // namespace PulseForgeEditor
