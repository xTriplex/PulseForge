#include "Editor/EditorSceneEntityCommands.h"

#include "Editor/EditorMeshPickingCache.h"
#include "Editor/EditorSceneHierarchyOperations.h"
#include "Editor/EditorSceneWorkspaceState.h"
#include "Editor/EditorSceneVisibilityCommands.h"
#include "Editor/ViewportMath.h"
#include "Scene/SceneSerializer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <unordered_set>
#include <utility>

namespace PulseForgeEditor
{
	void EditorSceneEntityCommands::CreateEntity(PulseForge::Scene* SceneValue, EditorSceneEntityCommandActions& Actions) const
	{
		if (!SceneValue) return;

		const auto Recorded = Actions.RunEditorSceneEdit("Create Entity",
			[SceneValue, &Actions]() -> std::expected<void, PulseForge::SceneError>
			{
				auto Created = SceneValue->CreateEntity();
				if (!Created) return std::unexpected(Created.error());
				Actions.SelectSingleEditorEntity(Created->GetUUID());
				return {};
			});
		if (!Recorded) Actions.ReportEditorSceneCommandError("Entity creation failed: " + Recorded.error());
	}

	void EditorSceneEntityCommands::DuplicateSelectedEntities(
		PulseForge::Scene* SceneValue, EditorSceneWorkspaceState& Workspace, EditorSceneEntityCommandActions& Actions) const
	{
		if (!SceneValue || Workspace.Selection().GetEntities().empty()) return;

		auto Roots = EditorSceneHierarchyOperations::GetTopLevelSelection(*SceneValue, Workspace.Selection().GetEntities());
		if (!Roots)
		{
			Actions.ReportEditorSceneCommandError("Could not duplicate the selection: " + Roots.error());
			return;
		}

		std::vector<PulseForge::UUID> DuplicatedRoots;
		const auto Recorded = Actions.RunEditorSceneEdit("Duplicate Entities",
			[SceneValue, Roots = std::move(*Roots), &DuplicatedRoots, &Actions]() -> std::expected<void, PulseForge::SceneError>
			{
				auto Result = EditorSceneHierarchyOperations::DuplicateSubtrees(*SceneValue, Roots);
				if (!Result) return std::unexpected(Result.error());
				DuplicatedRoots = std::move(*Result);
				Actions.SetEditorSceneSelection(DuplicatedRoots);
				return {};
			});
		if (!Recorded) Actions.ReportEditorSceneCommandError("Entity duplication failed: " + Recorded.error());
		else Actions.SetEditorSceneCommandStatus("Duplicated " + std::to_string(DuplicatedRoots.size()) + " selected hierarchy root(s).");
	}

	void EditorSceneEntityCommands::DeleteSelectedEntities(
		PulseForge::Scene* SceneValue, EditorSceneWorkspaceState& Workspace, EditorSceneEntityCommandActions& Actions) const
	{
		if (!SceneValue || Workspace.Selection().GetEntities().empty()) return;

		auto Roots = EditorSceneHierarchyOperations::GetTopLevelSelection(*SceneValue, Workspace.Selection().GetEntities());
		if (!Roots)
		{
			Actions.ReportEditorSceneCommandError("Could not delete the selection: " + Roots.error());
			return;
		}
		auto Subtree = EditorSceneHierarchyOperations::CollectSubtrees(*Roots);
		if (!Subtree)
		{
			Actions.ReportEditorSceneCommandError("Could not delete the selection: " + Subtree.error());
			return;
		}

		const size_t DeleteCount = Subtree->size();
		const auto Recorded = Actions.RunEditorSceneEdit("Delete Entities",
			[SceneValue, &Workspace, &Actions, Subtree = std::move(*Subtree)]() -> std::expected<void, PulseForge::SceneError>
			{
				for (auto Entity = Subtree.rbegin(); Entity != Subtree.rend(); ++Entity)
				{
					const auto Target = SceneValue->FindEntity(Entity->GetUUID());
					if (!Target) continue;
					Workspace.Visibility().Remove(Entity->GetUUID());
					if (auto Deleted = SceneValue->DestroyEntity(*Target); !Deleted) return std::unexpected(Deleted.error());
				}
				Actions.SelectSingleEditorEntity(std::nullopt);
				return {};
			});
		if (!Recorded) Actions.ReportEditorSceneCommandError("Entity deletion failed: " + Recorded.error());
		else Actions.SetEditorSceneCommandStatus("Deleted " + std::to_string(DeleteCount) + " entities.");
	}

	bool EditorSceneEntityCommands::CopySelectedEntities(PulseForge::Scene* SceneValue, const PulseForge::UUID& ProjectID,
		std::optional<PulseForge::AssetID> SceneAsset, EditorSceneWorkspaceState& Workspace, EditorSceneCommandActions& Actions)
	{
		if (!SceneValue || Workspace.Selection().GetEntities().empty()) return false;

		auto Roots = EditorSceneHierarchyOperations::GetTopLevelSelection(*SceneValue, Workspace.Selection().GetEntities());
		if (!Roots)
		{
			Actions.ReportEditorSceneCommandError("Could not copy the selection: " + Roots.error());
			return false;
		}
		auto Subtree = EditorSceneHierarchyOperations::CollectSubtrees(*Roots);
		if (!Subtree)
		{
			Actions.ReportEditorSceneCommandError("Could not copy the selection: " + Subtree.error());
			return false;
		}

		auto Serialized = PulseForge::SceneSerializer::Serialize(*SceneValue);
		if (!Serialized)
		{
			Actions.ReportEditorSceneCommandError("Could not serialize the selection for the clipboard: " + Serialized.error().Message);
			return false;
		}

		std::vector<PulseForge::UUID> RootIdentifiers;
		RootIdentifiers.reserve(Roots->size());
		for (const PulseForge::Entity& Root : *Roots)
			RootIdentifiers.push_back(Root.GetUUID());
		std::vector<PulseForge::UUID> IncludedIdentifiers;
		IncludedIdentifiers.reserve(Subtree->size());
		for (const PulseForge::Entity& Entity : *Subtree)
			IncludedIdentifiers.push_back(Entity.GetUUID());

		auto Clipboard = EditorEntityClipboard::Capture(ProjectID, SceneAsset, RootIdentifiers, IncludedIdentifiers, *Serialized);
		if (!Clipboard)
		{
			Actions.ReportEditorSceneCommandError("Could not encode the selection for the clipboard: " + Clipboard.error());
			return false;
		}

		m_Clipboard = std::move(*Clipboard);
		Actions.SetEditorSceneCommandStatus("Copied " + std::to_string(Subtree->size()) + " entities.");
		return true;
	}

	void EditorSceneEntityCommands::CutSelectedEntities(PulseForge::Scene* SceneValue, const PulseForge::UUID& ProjectID,
		std::optional<PulseForge::AssetID> SceneAsset, EditorSceneWorkspaceState& Workspace, EditorSceneEntityCommandActions& Actions)
	{
		if (Workspace.Selection().GetEntities().empty()) return;
		if (CopySelectedEntities(SceneValue, ProjectID, SceneAsset, Workspace, Actions))
			DeleteSelectedEntities(SceneValue, Workspace, Actions);
	}

	void EditorSceneEntityCommands::PasteEntities(PulseForge::Scene* SceneValue, const PulseForge::UUID& ProjectID,
		std::optional<PulseForge::AssetID> SceneAsset, EditorSceneWorkspaceState& Workspace, bool RuntimeActive,
		const std::function<bool(const PulseForge::UUID&)>& IsAssetRegistered, EditorSceneEntityCommandActions& Actions) const
	{
		if (!SceneValue || !m_Clipboard.HasData() || RuntimeActive) return;
		if (!m_Clipboard.CanPaste(ProjectID))
		{
			Actions.ReportEditorSceneCommandWarning("The clipboard belongs to another project; copy the entities again in this project.");
			return;
		}

		auto Serialized = PulseForge::SceneSerializer::Serialize(*SceneValue);
		if (!Serialized)
		{
			Actions.ReportEditorSceneCommandError("Could not prepare the active scene for paste: " + Serialized.error().Message);
			return;
		}

		auto Prepared = m_Clipboard.PreparePaste(ProjectID, SceneAsset, *Serialized, IsAssetRegistered);
		if (!Prepared)
		{
			Actions.ReportEditorSceneCommandError("Paste failed: " + Prepared.error());
			return;
		}

		const size_t PastedCount = Prepared->EntityCount;
		const std::string MergedScene = std::move(Prepared->SerializedScene);
		const std::vector<PulseForge::UUID> NewRoots = std::move(Prepared->RootIdentifiers);
		const auto Recorded = Actions.RunEditorSceneEdit("Paste Entities",
			[SceneValue, &Actions, MergedScene, NewRoots]() -> std::expected<void, PulseForge::SceneError>
			{
				auto Result = PulseForge::SceneSerializer::Deserialize(MergedScene, *SceneValue);
				if (!Result)
					return std::unexpected(PulseForge::SceneError{ PulseForge::SceneErrorCode::InvalidEntity, Result.error().Message });
				Actions.SetEditorSceneSelection(NewRoots);
				return {};
			});
		if (!Recorded)
		{
			Actions.ReportEditorSceneCommandError("Paste failed: " + Recorded.error());
			return;
		}
		Actions.SetEditorSceneCommandStatus("Pasted " + std::to_string(PastedCount) + " entities.");
	}

	void EditorSceneEntityCommands::ReparentEntity(PulseForge::Scene* SceneValue, const PulseForge::UUID& ChildIdentifier,
		std::optional<PulseForge::UUID> ParentIdentifier, EditorSceneCommandActions& Actions) const
	{
		if (!SceneValue) return;
		const auto Child = SceneValue->FindEntity(ChildIdentifier);
		if (!Child)
		{
			Actions.ReportEditorSceneCommandError("The hierarchy entity no longer exists.");
			return;
		}
		if (ParentIdentifier && !SceneValue->FindEntity(*ParentIdentifier))
		{
			Actions.ReportEditorSceneCommandError("The selected parent entity no longer exists.");
			return;
		}

		const auto Recorded = Actions.RunEditorSceneEdit("Reparent Entity",
			[SceneValue, Child = *Child, ParentIdentifier]() -> std::expected<void, PulseForge::SceneError>
			{
				if (ParentIdentifier)
				{
					const auto Parent = SceneValue->FindEntity(*ParentIdentifier);
					if (!Parent)
						return std::unexpected(PulseForge::SceneError{
							PulseForge::SceneErrorCode::InvalidEntity, "The selected parent entity no longer exists" });
					return Child.SetParent(*Parent);
				}
				return Child.ClearParent();
			});
		if (!Recorded) Actions.ReportEditorSceneCommandError("Entity reparenting failed: " + Recorded.error());
	}

	void EditorSceneEntityCommands::SnapSelectionToSurface(PulseForge::Scene* SceneValue, EditorSceneWorkspaceState& Workspace,
		bool RuntimeActive, const EditorMeshGeometryResolver& ResolveMesh, EditorSceneEntityCommandActions& Actions) const
	{
		if (!SceneValue || RuntimeActive || Workspace.Selection().GetEntities().empty()) return;
		if (!ResolveMesh)
		{
			Actions.ReportEditorSceneCommandError("Surface snapping requires an active CPU mesh geometry source.");
			return;
		}
		auto Roots = EditorSceneHierarchyOperations::GetTopLevelSelection(*SceneValue, Workspace.Selection().GetEntities());
		if (!Roots || Roots->empty()) return;

		auto MovingSubtrees = EditorSceneHierarchyOperations::CollectSubtrees(*Roots);
		if (!MovingSubtrees)
		{
			Actions.ReportEditorSceneCommandError(
				"Could not inspect the selected hierarchy for surface snapping: " + MovingSubtrees.error());
			return;
		}
		std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash> ExcludedEntities;
		for (const PulseForge::Entity& Entity : *MovingSubtrees)
			ExcludedEntities.insert(Entity.GetUUID());

		std::vector<PickableMesh> CandidateMeshes;
		for (const PulseForge::Entity& Entity : SceneValue->GetEntities())
		{
			if (ExcludedEntities.contains(Entity.GetUUID()) ||
				!EditorSceneVisibilityCommands::IsEntityEffectivelyVisible(Entity, Workspace.Visibility()))
				continue;
			const auto MeshRenderer = Entity.GetMeshRenderer();
			if (!MeshRenderer || !MeshRenderer->has_value()) continue;
			const auto World = Entity.GetWorldMatrix();
			if (!World || !Detail::IsFinite(*World)) continue;
			auto Geometry = ResolveMesh(MeshRenderer->value().MeshAsset);
			if (!Geometry)
			{
				Actions.ReportEditorSceneCommandWarning(Geometry.error());
				continue;
			}
			CandidateMeshes.push_back({ Entity.GetUUID(), *World, (*Geometry)->Positions, (*Geometry)->Indices });
		}
		if (CandidateMeshes.empty())
		{
			Actions.SetEditorSceneCommandStatus("No visible mesh surfaces are available below the selection.");
			return;
		}

		std::vector<std::pair<PulseForge::UUID, PulseForge::TransformComponent>> UpdatedTransforms;
		constexpr size_t MaximumContactCandidates = 2048;
		constexpr size_t MaximumContactSamples = 128;
		for (const PulseForge::Entity& Root : *Roots)
		{
			const std::array RootOnly{ Root };
			auto Subtree = EditorSceneHierarchyOperations::CollectSubtrees(RootOnly);
			if (!Subtree)
			{
				Actions.ReportEditorSceneCommandError("Could not inspect a selected hierarchy for surface snapping: " + Subtree.error());
				return;
			}
			std::vector<glm::vec3> ContactPoints;
			for (const PulseForge::Entity& Entity : *Subtree)
			{
				if (!EditorSceneVisibilityCommands::IsEntityEffectivelyVisible(Entity, Workspace.Visibility())) continue;
				const auto MeshRenderer = Entity.GetMeshRenderer();
				if (!MeshRenderer || !MeshRenderer->has_value()) continue;
				const auto World = Entity.GetWorldMatrix();
				if (!World || !Detail::IsFinite(*World)) continue;
				auto Geometry = ResolveMesh(MeshRenderer->value().MeshAsset);
				if (!Geometry) continue;
				for (const glm::vec3& Vertex : (*Geometry)->Positions)
				{
					const glm::vec3 Position(*World * glm::vec4(Vertex, 1.0f));
					if (!Detail::IsFinite(Position)) continue;
					if (ContactPoints.size() < MaximumContactCandidates && std::none_of(ContactPoints.begin(), ContactPoints.end(),
																			   [&Position](const glm::vec3& Existing)
																			   {
																				   const glm::vec3 Difference = Existing - Position;
																				   return glm::dot(Difference, Difference) < 1.0e-6f;
																			   }))
						ContactPoints.push_back(Position);
				}
			}
			if (ContactPoints.empty())
			{
				if (const auto World = Root.GetWorldMatrix(); World && Detail::IsFinite(*World))
					ContactPoints.push_back(glm::vec3((*World)[3]));
			}
			if (ContactPoints.size() > MaximumContactSamples)
			{
				std::vector<glm::vec3> Samples;
				const size_t Stride = (ContactPoints.size() + MaximumContactSamples - 1) / MaximumContactSamples;
				for (size_t Index = 0; Index < ContactPoints.size(); Index += Stride)
					Samples.push_back(ContactPoints[Index]);
				ContactPoints = std::move(Samples);
			}

			float BestDeltaY = -(std::numeric_limits<float>::infinity)();
			for (const glm::vec3& Point : ContactPoints)
			{
				constexpr float RayStartOffset = 0.005f;
				const auto Hit =
					RaycastSupportingSurface({ Point + glm::vec3(0.0f, RayStartOffset, 0.0f), { 0.0f, -1.0f, 0.0f } }, CandidateMeshes);
				if (!Hit) continue;
				const float DeltaY = Hit->Position.y - Point.y;
				if (DeltaY <= RayStartOffset) BestDeltaY = (std::max)(BestDeltaY, DeltaY);
			}
			if (!std::isfinite(BestDeltaY) || BestDeltaY >= -0.001f) continue;

			const auto Transform = Root.GetTransform();
			const auto World = Root.GetWorldMatrix();
			const auto Parent = Root.GetParent();
			if (!Transform || !World || !Parent) continue;
			glm::mat4 ParentWorld(1.0f);
			if (Parent->has_value())
			{
				const auto ParentTransform = Parent->value().GetWorldMatrix();
				if (!ParentTransform || !Detail::IsFinite(*ParentTransform) || std::abs(glm::determinant(*ParentTransform)) < 1.0e-8f)
					continue;
				ParentWorld = *ParentTransform;
			}
			const glm::vec3 WorldPosition((*World)[3]);
			const glm::vec3 NewWorldPosition = WorldPosition + glm::vec3(0.0f, BestDeltaY, 0.0f);
			PulseForge::TransformComponent Updated = *Transform;
			Updated.Translation = glm::vec3(glm::inverse(ParentWorld) * glm::vec4(NewWorldPosition, 1.0f));
			if (!Detail::IsFinite(Updated.Translation)) continue;
			UpdatedTransforms.emplace_back(Root.GetUUID(), Updated);
		}

		if (UpdatedTransforms.empty())
		{
			Actions.SetEditorSceneCommandStatus("No supporting surface was found below the selection.");
			return;
		}
		const auto Recorded = Actions.RunEditorSceneEdit("Snap Selection to Surface",
			[SceneValue, UpdatedTransforms]() -> std::expected<void, PulseForge::SceneError>
			{
				for (const auto& [Identifier, Transform] : UpdatedTransforms)
				{
					const auto Entity = SceneValue->FindEntity(Identifier);
					if (!Entity)
						return std::unexpected(PulseForge::SceneError{
							PulseForge::SceneErrorCode::InvalidEntity, "A selected entity disappeared during surface snapping" });
					if (auto Result = Entity->SetTransform(Transform); !Result) return std::unexpected(Result.error());
				}
				return {};
			});
		if (!Recorded) Actions.ReportEditorSceneCommandError("Surface snapping failed: " + Recorded.error());
		else
			Actions.SetEditorSceneCommandStatus(
				"Snapped " + std::to_string(UpdatedTransforms.size()) + " selected entities to supporting surfaces.");
	}

	bool EditorSceneEntityCommands::CanPaste(const PulseForge::UUID& ProjectID) const noexcept
	{
		return m_Clipboard.CanPaste(ProjectID);
	}
} // namespace PulseForgeEditor
