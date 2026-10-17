#include "TestSupport.h"

#include "Editor/EditorMeshPickingGeometry.h"
#include "Editor/EditorSceneViewportInteraction.h"
#include "Editor/EditorSceneWorkspaceState.h"
#include "Scene/Components/MeshRendererComponent.h"
#include "Scene/Scene.h"

#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

void RunEditorSceneViewportInteractionTests(TestRunner& Tests)
{
	using namespace PulseForge;
	using namespace PulseForgeEditor;
	const AssetID MeshID{ 201, 1 };
	const AssetID MissingMeshID{ 202, 2 };
	const EditorMeshPickingGeometry Geometry{ .Positions = { { -1.0f, -1.0f, 0.0f }, { 1.0f, -1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } },
		.Indices = { 0, 1, 2 } };
	const EditorMeshGeometryResolver ResolveMesh = [&](const AssetID& Asset) -> std::expected<const EditorMeshPickingGeometry*, std::string>
	{
		if (Asset == MeshID) return &Geometry;
		return std::unexpected("mesh geometry is unavailable");
	};
	const glm::mat4 View = glm::lookAtRH(glm::vec3(0.0f, 0.0f, 3.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
	const glm::mat4 Projection = glm::perspectiveRH_ZO(glm::radians(45.0f), 1.0f, 0.1f, 100.0f);
	const glm::mat4 ViewProjection = Projection * View;
	const ViewportImageRect ImageRect{ { 0.0f, 0.0f }, { 100.0f, 100.0f } };
	EditorSceneViewportInteraction Interaction;
	EditorSceneWorkspaceState Workspace;
	Scene SceneValue;
	auto Target = SceneValue.CreateEntity("Pick Target");
	auto Missing = SceneValue.CreateEntity("Missing Geometry");
	PF_CHECK(Tests, Target && Missing);
	if (!Target || !Missing) return;
	PF_CHECK(Tests, Target->SetMeshRenderer(MeshRendererComponent{ MeshID, std::nullopt }));
	PF_CHECK(Tests, Missing->SetMeshRenderer(MeshRendererComponent{ MissingMeshID, std::nullopt }));
	auto MissingTransform = Missing->GetTransform();
	PF_CHECK(Tests, MissingTransform.has_value());
	if (!MissingTransform) return;
	MissingTransform->Translation.z = 1.0f;
	PF_CHECK(Tests, Missing->SetTransform(*MissingTransform));

	Workspace.Selection().SelectSingle(Missing->GetUUID());
	auto Picked = Interaction.PickEntityAt(
		&SceneValue, Workspace, false, true, ImageRect, ViewProjection, { 0.0f, 0.0f, 3.0f }, { 50.0f, 50.0f }, false, false, ResolveMesh);
	PF_CHECK(Tests, Picked && Picked->size() == 1 && Picked->front() == "mesh geometry is unavailable");
	PF_CHECK(Tests, Workspace.Selection().GetEntities().size() == 1 && Workspace.Selection().GetPrimary() == Target->GetUUID());

	Picked = Interaction.PickEntityAt(
		&SceneValue, Workspace, false, true, ImageRect, ViewProjection, { 0.0f, 0.0f, 3.0f }, { 50.0f, 50.0f }, true, false, ResolveMesh);
	PF_CHECK(Tests, Picked && Workspace.Selection().GetEntities().empty());
	Workspace.Selection().SelectSingle(Missing->GetUUID());
	Picked = Interaction.PickEntityAt(
		&SceneValue, Workspace, false, true, ImageRect, ViewProjection, { 0.0f, 0.0f, 3.0f }, { 50.0f, 50.0f }, false, true, ResolveMesh);
	PF_CHECK(Tests, Picked && Workspace.Selection().GetEntities().size() == 2);

	Workspace.Visibility().SetVisible(Target->GetUUID(), false);
	Workspace.Selection().SelectSingle(Missing->GetUUID());
	Picked = Interaction.PickEntityAt(
		&SceneValue, Workspace, false, true, ImageRect, ViewProjection, { 0.0f, 0.0f, 3.0f }, { 50.0f, 50.0f }, false, false, ResolveMesh);
	PF_CHECK(Tests, Picked && Workspace.Selection().GetEntities().empty());

	Workspace.Selection().SelectSingle(Target->GetUUID());
	Workspace.Visibility().SetVisible(Target->GetUUID(), true);
	auto TargetTransform = Target->GetTransform();
	PF_CHECK(Tests, TargetTransform.has_value());
	if (!TargetTransform) return;
	TargetTransform->Translation = { 3.0f, 2.0f, 0.0f };
	PF_CHECK(Tests, Target->SetTransform(*TargetTransform));
	auto Focus = Interaction.FindSelectionFocus(SceneValue, Workspace, ResolveMesh);
	PF_CHECK(Tests, Focus && Focus->Focus.has_value());
	if (Focus && Focus->Focus)
	{
		PF_CHECK(Tests, glm::length(Focus->Focus->Center - glm::vec3(3.0f, 2.0f, 0.0f)) < 1.0e-4f);
		PF_CHECK(Tests, std::abs(Focus->Focus->Radius - std::sqrt(2.0f)) < 1.0e-4f);
	}

	const EditorMeshGeometryResolver MissingGeometry = [](const AssetID&) -> std::expected<const EditorMeshPickingGeometry*, std::string>
	{ return std::unexpected("mesh geometry unavailable for framing"); };
	Focus = Interaction.FindSelectionFocus(SceneValue, Workspace, MissingGeometry);
	PF_CHECK(Tests, Focus && Focus->Focus && Focus->Warnings.size() == 1);
	if (Focus && Focus->Focus)
	{
		PF_CHECK(Tests, glm::length(Focus->Focus->Center - glm::vec3(3.0f, 2.0f, 0.0f)) < 1.0e-4f);
		PF_CHECK(Tests, std::abs(Focus->Focus->Radius - 0.35f) < 1.0e-4f);
	}

	Workspace.Selection().SelectSingle(Target->GetUUID());
	Picked = Interaction.PickEntityAt(
		&SceneValue, Workspace, true, true, ImageRect, ViewProjection, { 0.0f, 0.0f, 3.0f }, { 50.0f, 50.0f }, false, false, ResolveMesh);
	PF_CHECK(Tests, Picked && Workspace.Selection().GetEntities().empty());
	Workspace.Selection().SelectSingle(Target->GetUUID());
	Picked = Interaction.PickEntityAt(
		&SceneValue, Workspace, false, false, ImageRect, ViewProjection, { 0.0f, 0.0f, 3.0f }, { 50.0f, 50.0f }, false, false, ResolveMesh);
	PF_CHECK(Tests, Picked && Workspace.Selection().GetEntities().empty());
}
