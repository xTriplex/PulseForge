#include "Editor/EditorSceneViewportInteraction.h"

#include "Editor/EditorSceneHierarchyOperations.h"
#include "Editor/EditorSceneVisibilityCommands.h"
#include "Editor/EditorSceneWorkspaceState.h"
#include "Scene/Scene.h"
#include "Scene/SceneRenderSnapshot.h"

#include <algorithm>
#include <limits>

#include <glm/gtc/matrix_transform.hpp>

namespace PulseForgeEditor
{
	std::expected<std::vector<std::string>, std::string> EditorSceneViewportInteraction::PickEntityAt(PulseForge::Scene* SceneValue,
		EditorSceneWorkspaceState& Workspace, bool RuntimeActive, bool ViewProjectionValid, const ViewportImageRect& ImageRect,
		const glm::mat4& ViewProjection, const glm::vec3& CameraPosition, const glm::vec2& MousePosition, bool Control, bool Shift,
		const EditorMeshGeometryResolver& ResolveMesh) const
	{
		if (!SceneValue || RuntimeActive || !ViewProjectionValid || !Detail::IsFinite(ViewProjection))
		{
			Workspace.Selection().Clear();
			return std::vector<std::string>{};
		}

		const auto Ray = MakeViewportRay(MousePosition, ImageRect, ViewProjection);
		if (!Ray) return std::vector<std::string>{};

		auto Snapshot = PulseForge::SceneRenderSnapshotBuilder::BuildForView(*SceneValue, ViewProjection, CameraPosition);
		if (!Snapshot) return std::unexpected("Viewport picking could not inspect the scene: " + Snapshot.error().Message);

		std::erase_if(Snapshot->Meshes,
			[&SceneValue, &Workspace](const PulseForge::SceneMeshInstance& Instance)
			{
				const auto Entity = SceneValue->FindEntity(Instance.Entity);
				return !Entity || !EditorSceneVisibilityCommands::IsEntityEffectivelyVisible(*Entity, Workspace.Visibility());
			});

		std::vector<PickableMesh> Meshes;
		Meshes.reserve(Snapshot->Meshes.size());
		std::vector<std::string> Warnings;
		for (const PulseForge::SceneMeshInstance& Instance : Snapshot->Meshes)
		{
			if (!ResolveMesh)
			{
				Warnings.emplace_back("Viewport picking has no CPU mesh geometry source.");
				break;
			}
			const auto Geometry = ResolveMesh(Instance.MeshAsset);
			if (!Geometry)
			{
				Warnings.push_back(Geometry.error());
				continue;
			}
			Meshes.push_back({ Instance.Entity, Instance.WorldTransform, (*Geometry)->Positions, (*Geometry)->Indices });
		}

		const auto Hit = RaycastMeshes(*Ray, Meshes);
		if (Hit)
		{
			if (Control) Workspace.Selection().Toggle(Hit->Entity);
			else if (Shift) Workspace.Selection().Add(Hit->Entity);
			else Workspace.Selection().SelectSingle(Hit->Entity);
		}
		else if (!Control && !Shift) Workspace.Selection().Clear();

		return Warnings;
	}

	std::expected<EditorViewportSelectionFocusResult, std::string> EditorSceneViewportInteraction::FindSelectionFocus(
		const PulseForge::Scene& SceneValue, const EditorSceneWorkspaceState& Workspace,
		const EditorMeshGeometryResolver& ResolveMesh) const
	{
		EditorViewportSelectionFocusResult Result;
		if (Workspace.Selection().GetEntities().empty()) return Result;

		auto Roots = EditorSceneHierarchyOperations::GetTopLevelSelection(SceneValue, Workspace.Selection().GetEntities());
		if (!Roots) return std::unexpected("Could not frame the selection: " + Roots.error());
		if (Roots->empty()) return Result;

		auto Subtree = EditorSceneHierarchyOperations::CollectSubtrees(*Roots);
		if (!Subtree) return std::unexpected("Could not frame the selection: " + Subtree.error());

		glm::vec3 Minimum((std::numeric_limits<float>::max)());
		glm::vec3 Maximum(-(std::numeric_limits<float>::max)());
		bool HasBounds = false;
		for (const PulseForge::Entity& Entity : *Subtree)
		{
			if (!EditorSceneVisibilityCommands::IsEntityEffectivelyVisible(Entity, Workspace.Visibility())) continue;

			const auto World = Entity.GetWorldMatrix();
			if (!World || !Detail::IsFinite(*World)) continue;
			const auto MeshRenderer = Entity.GetMeshRenderer();
			if (!MeshRenderer || !MeshRenderer->has_value()) continue;

			if (!ResolveMesh)
			{
				Result.Warnings.emplace_back("Selection framing has no CPU mesh geometry source.");
				continue;
			}
			const auto Geometry = ResolveMesh(MeshRenderer->value().MeshAsset);
			if (!Geometry)
			{
				Result.Warnings.push_back(Geometry.error());
				continue;
			}

			for (const glm::vec3& Vertex : (*Geometry)->Positions)
			{
				const glm::vec3 Position(*World * glm::vec4(Vertex, 1.0f));
				if (!Detail::IsFinite(Position)) continue;
				for (glm::length_t Axis = 0; Axis < 3; ++Axis)
				{
					Minimum[Axis] = (std::min)(Minimum[Axis], Position[Axis]);
					Maximum[Axis] = (std::max)(Maximum[Axis], Position[Axis]);
				}
				HasBounds = true;
			}
		}

		if (!HasBounds)
		{
			for (const PulseForge::Entity& Root : *Roots)
				if (const auto World = Root.GetWorldMatrix(); World && Detail::IsFinite(*World))
				{
					const glm::vec3 Position((*World)[3]);
					for (glm::length_t Axis = 0; Axis < 3; ++Axis)
					{
						Minimum[Axis] = (std::min)(Minimum[Axis], Position[Axis]);
						Maximum[Axis] = (std::max)(Maximum[Axis], Position[Axis]);
					}
					HasBounds = true;
				}
		}
		if (!HasBounds) return Result;

		Result.Focus = EditorViewportSelectionFocus{ (Minimum + Maximum) * 0.5f, (std::max)(glm::length(Maximum - Minimum) * 0.5f, 0.35f) };
		return Result;
	}
} // namespace PulseForgeEditor
