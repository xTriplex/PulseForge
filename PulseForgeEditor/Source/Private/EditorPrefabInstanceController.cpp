#include "Editor/EditorPrefabInstanceController.h"

#include "Assets/PrefabInstanceReconciler.h"
#include "Assets/Project.h"
#include "Editor/EditorDocumentManager.h"
#include "Editor/EditorPrefabDocumentEditor.h"

#include <algorithm>
#include <utility>

namespace PulseForgeEditor
{
	std::expected<std::optional<EditorPrefabInstancePanelStatus>, std::string> EditorPrefabInstanceController::GetStatus(
		const PulseForge::Entity& Entity)
	{
		PulseForge::Scene* const Scene = m_Host.GetPrefabScene();
		if (!Scene) return std::optional<EditorPrefabInstancePanelStatus>{};
		auto Root = PulseForge::PrefabInstanceReconciler::FindLinkedRoot(*Scene, Entity);
		if (!Root) return std::unexpected(Root.error().Message);
		if (!Root->has_value()) return std::optional<EditorPrefabInstancePanelStatus>{};
		auto Metadata = (**Root).GetPrefabInstance();
		if (!Metadata) return std::unexpected(Metadata.error().Message);
		if (!Metadata->has_value()) return std::unexpected("Linked prefab root metadata is missing");

		EditorPrefabInstancePanelStatus Status;
		Status.SourceAsset = (**Metadata).SourceAsset;
		Status.InstanceID = (**Metadata).InstanceID;
		Status.OverrideCount = (**Metadata).Overrides.size();
		Status.ConflictCount = (**Metadata).Conflicts.size();
		Status.Conflicts = (**Metadata).Conflicts;
		const auto SourceMapping = std::find_if((**Metadata).SourceEntities.begin(), (**Metadata).SourceEntities.end(),
			[&Entity](const PulseForge::PrefabEntityMapping& Mapping) { return Mapping.SceneEntity == Entity.GetUUID(); });
		if (SourceMapping != (**Metadata).SourceEntities.end())
		{
			const PulseForge::PrefabEntityAddress Address{ SourceMapping->SourceKey, false };
			Status.HasSelectedSourceOverrides = std::any_of((**Metadata).Overrides.begin(), (**Metadata).Overrides.end(),
				[&Address](const PulseForge::PrefabOverrideRecord& Override) { return Override.Entity == Address; });
		}
		const PulseForge::Project* const Project = m_Host.GetPrefabProject();
		if (Project && m_PendingProjectID == Project->GetDescription().ID &&
			m_PendingSceneSessionGeneration == m_Host.GetPrefabSceneSessionGeneration() && m_PendingReconcile &&
			m_PendingReconcile->GetInstanceID() == Status.InstanceID)
		{
			Status.HasPendingPlan = true;
			Status.CanApply = m_PendingReconcile->CanApply();
			Status.PlanSummary = m_PendingReconcile->GetSummary();
			Status.Conflicts = m_PendingReconcile->GetConflicts();
			Status.ConflictCount = Status.Conflicts.size();
		}
		return std::optional<EditorPrefabInstancePanelStatus>{ std::move(Status) };
	}

	void EditorPrefabInstanceController::RecordMissingSourceConflict(
		const PulseForge::UUID& RootID,
		const PulseForge::UUID& ProjectID,
		const PulseForge::PrefabInstanceComponent& ExpectedInstance,
		uint64_t SceneSessionGeneration,
		std::string Message)
	{
		if (m_Host.IsPrefabRuntimeActive())
		{
			m_Host.ReportPrefabWarning("Stop Play before recording a missing prefab source conflict in the authored scene.");
			return;
		}
		auto Recorded = m_Host.RunPrefabSceneEdit("Record Missing Prefab Source Conflict",
			[this, RootID, ProjectID, ExpectedInstance, SceneSessionGeneration, Message = std::move(Message)]()
				-> std::expected<void, PulseForge::SceneError>
			{
				PulseForge::Project* const ActiveProject = m_Host.GetPrefabProject();
				PulseForge::Scene* const ActiveScene = m_Host.GetPrefabScene();
				if (!ActiveProject || ActiveProject->GetDescription().ID != ProjectID || !ActiveScene ||
					m_Host.GetPrefabSceneSessionGeneration() != SceneSessionGeneration)
					return std::unexpected(PulseForge::SceneError{ PulseForge::SceneErrorCode::InvalidPrefabInstance,
						"The project or scene changed before conflict recording" });
				const auto RootNow = ActiveScene->FindEntity(RootID);
				const auto MetadataNow = RootNow ? RootNow->GetPrefabInstance() :
					std::expected<std::optional<PulseForge::PrefabInstanceComponent>, PulseForge::SceneError>{ std::unexpected(
						PulseForge::SceneError{ PulseForge::SceneErrorCode::InvalidPrefabInstance, "Linked root is unavailable" }) };
				if (!RootNow || !MetadataNow || !MetadataNow->has_value() || **MetadataNow != ExpectedInstance)
					return std::unexpected(PulseForge::SceneError{ PulseForge::SceneErrorCode::InvalidPrefabInstance,
						"Linked instance changed before its missing-source conflict could be recorded" });
				auto Conflict = PulseForge::PrefabInstanceReconciler::RecordMissingSourceAssetConflict(*RootNow, Message);
				if (!Conflict)
					return std::unexpected(PulseForge::SceneError{
						PulseForge::SceneErrorCode::InvalidPrefabInstance, Conflict.error().Message });
				return {};
			});
		if (!Recorded)
		{
			m_Host.ReportPrefabError("Could not record the missing prefab source conflict: " + Recorded.error());
			return;
		}
		m_PendingReconcile.reset();
		m_PendingProjectID.reset();
		m_PendingSceneSessionGeneration.reset();
		m_Host.UpdatePrefabSceneDirty();
		m_Host.ReportPrefabWarning(
			"The prefab source is missing. The materialized instance was preserved and a recoverable scene conflict was recorded.");
	}

	std::expected<EditorPrefabInstancePanelStatus, std::string> EditorPrefabInstanceController::PlanUpdate(
		const PulseForge::Entity& Entity)
	{
		PulseForge::Project* const Project = m_Host.GetPrefabProject();
		PulseForge::Scene* const Scene = m_Host.GetPrefabScene();
		if (!Project || !Scene) return std::unexpected("Open a project and scene before updating a prefab instance");
		auto Root = PulseForge::PrefabInstanceReconciler::FindLinkedRoot(*Scene, Entity);
		if (!Root) return std::unexpected(Root.error().Message);
		if (!Root->has_value()) return std::unexpected("Selected entity is not part of a linked prefab instance");
		auto Metadata = (**Root).GetPrefabInstance();
		if (!Metadata || !Metadata->has_value()) return std::unexpected("Linked prefab root metadata is unavailable");
		const PulseForge::UUID RootID = (**Root).GetUUID();
		const PulseForge::UUID ExpectedProjectID = Project->GetDescription().ID;
		const PulseForge::PrefabInstanceComponent ExpectedInstance = **Metadata;
		const uint64_t ExpectedSceneSession = m_Host.GetPrefabSceneSessionGeneration();
		m_Host.QueuePrefabOperation([this, RootID, ExpectedProjectID, ExpectedInstance, ExpectedSceneSession]
		{
			PulseForge::Project* const CurrentProject = m_Host.GetPrefabProject();
			PulseForge::Scene* const CurrentScene = m_Host.GetPrefabScene();
			if (!CurrentProject || !CurrentScene || CurrentProject->GetDescription().ID != ExpectedProjectID ||
				m_Host.GetPrefabSceneSessionGeneration() != ExpectedSceneSession)
			{
				m_Host.ReportPrefabWarning("The prefab update was cancelled because its project or scene changed before planning.");
				return;
			}
			const auto CurrentRoot = CurrentScene->FindEntity(RootID);
			if (!CurrentRoot)
			{
				m_Host.ReportPrefabWarning("The linked prefab root disappeared before its update could be planned.");
				return;
			}
			const auto CurrentMetadata = CurrentRoot->GetPrefabInstance();
			if (!CurrentMetadata || !CurrentMetadata->has_value() || **CurrentMetadata != ExpectedInstance)
			{
				m_Host.ReportPrefabWarning("The linked prefab instance changed before planning; select it and plan again.");
				return;
			}
			m_PendingReconcile.reset();
			m_PendingProjectID.reset();
			m_PendingSceneSessionGeneration.reset();
			auto Planned = PulseForge::PrefabAssetService::PlanReconcile(
				ExpectedInstance.SourceAsset, CurrentProject->GetRootPath(), CurrentProject->GetAssetRegistry(), *CurrentScene, *CurrentRoot);
			if (!Planned)
			{
				if (Planned.error().Code == PulseForge::PrefabAssetErrorCode::AssetNotFound ||
					Planned.error().Code == PulseForge::PrefabAssetErrorCode::SourceFileMissing)
				{
					RecordMissingSourceConflict(RootID, ExpectedProjectID, ExpectedInstance, ExpectedSceneSession,
						"The prefab source asset is unavailable; the materialized instance was preserved: " + Planned.error().Message);
					return;
				}
				m_Host.ReportPrefabError("Linked prefab update planning failed: " + Planned.error().Message);
				return;
			}
			m_PendingReconcile.emplace(std::move(*Planned));
			m_PendingProjectID = ExpectedProjectID;
			m_PendingSceneSessionGeneration = ExpectedSceneSession;
			m_Host.ReportPrefabStatus("Planned a linked prefab update at the safe scene-authoring boundary.");
		});
		auto Status = GetStatus(Entity);
		if (!Status || !Status->has_value())
			return std::unexpected(Status ? "Linked prefab identity changed while planning" : Status.error());
		m_Host.ReportPrefabStatus("Queued linked prefab update planning for the next authoring boundary.");
		return std::move(**Status);
	}

	std::expected<EditorPrefabInstancePanelStatus, std::string> EditorPrefabInstanceController::ApplyUpdate(
		const PulseForge::Entity& Entity)
	{
		PulseForge::Project* const Project = m_Host.GetPrefabProject();
		PulseForge::Scene* const Scene = m_Host.GetPrefabScene();
		if (!Project || !Scene || !m_PendingReconcile || !m_PendingProjectID ||
			*m_PendingProjectID != Project->GetDescription().ID || !m_PendingSceneSessionGeneration ||
			*m_PendingSceneSessionGeneration != m_Host.GetPrefabSceneSessionGeneration())
		{
			m_PendingReconcile.reset();
			m_PendingProjectID.reset();
			m_PendingSceneSessionGeneration.reset();
			return std::unexpected("Plan a prefab update in the current project and scene before applying it");
		}
		auto Root = PulseForge::PrefabInstanceReconciler::FindLinkedRoot(*Scene, Entity);
		if (!Root) return std::unexpected(Root.error().Message);
		if (!Root->has_value()) return std::unexpected("Selected entity is no longer part of a linked prefab instance");
		auto Metadata = (**Root).GetPrefabInstance();
		if (!Metadata || !Metadata->has_value() || (**Metadata).InstanceID != m_PendingReconcile->GetInstanceID() ||
			(**Metadata).SourceAsset != m_PendingReconcile->GetSourceAsset())
			return std::unexpected("The selected linked instance changed after planning; plan the update again");

		const PulseForge::UUID RootID = (**Root).GetUUID();
		const PulseForge::UUID ExpectedInstanceID = (**Metadata).InstanceID;
		const PulseForge::AssetID ExpectedSourceAsset = (**Metadata).SourceAsset;
		const PulseForge::PrefabInstanceComponent ExpectedInstance = **Metadata;
		const PulseForge::UUID ExpectedProjectID = Project->GetDescription().ID;
		const uint64_t ExpectedSceneSession = m_Host.GetPrefabSceneSessionGeneration();
		m_Host.QueuePrefabOperation([this, RootID, ExpectedProjectID, ExpectedInstanceID, ExpectedSourceAsset,
			ExpectedInstance, ExpectedSceneSession]
		{
			PulseForge::Project* const CurrentProject = m_Host.GetPrefabProject();
			PulseForge::Scene* const CurrentScene = m_Host.GetPrefabScene();
			if (!m_PendingReconcile || !CurrentProject || !CurrentScene ||
				CurrentProject->GetDescription().ID != ExpectedProjectID ||
				m_Host.GetPrefabSceneSessionGeneration() != ExpectedSceneSession)
			{
				m_PendingReconcile.reset();
				m_PendingProjectID.reset();
				m_PendingSceneSessionGeneration.reset();
				m_Host.ReportPrefabWarning("The pending prefab update was superseded before application.");
				return;
			}
			const auto CurrentRoot = CurrentScene->FindEntity(RootID);
			const auto CurrentMetadata = CurrentRoot ? CurrentRoot->GetPrefabInstance() :
				std::expected<std::optional<PulseForge::PrefabInstanceComponent>, PulseForge::SceneError>{ std::unexpected(
					PulseForge::SceneError{ PulseForge::SceneErrorCode::InvalidPrefabInstance, "Linked root is unavailable" }) };
			if (!CurrentRoot || !CurrentMetadata || !CurrentMetadata->has_value() ||
				(**CurrentMetadata).InstanceID != ExpectedInstanceID || (**CurrentMetadata).SourceAsset != ExpectedSourceAsset ||
				m_PendingReconcile->GetInstanceID() != ExpectedInstanceID || m_PendingReconcile->GetSourceAsset() != ExpectedSourceAsset ||
				**CurrentMetadata != ExpectedInstance || !m_PendingProjectID || *m_PendingProjectID != ExpectedProjectID ||
				!m_PendingSceneSessionGeneration || *m_PendingSceneSessionGeneration != ExpectedSceneSession)
			{
				m_PendingReconcile.reset();
				m_PendingProjectID.reset();
				m_PendingSceneSessionGeneration.reset();
				m_Host.ReportPrefabWarning("The linked prefab instance changed after planning; plan the update again.");
				return;
			}

			const std::string Description = m_PendingReconcile->CanApply()
				? "Update Linked Prefab Instance" : "Record Linked Prefab Conflicts";
			std::optional<PulseForge::PrefabAssetErrorCode> ApplyErrorCode;
			auto Applied = m_Host.RunPrefabSceneEdit(Description,
				[this, ExpectedProjectID, ExpectedSceneSession, &ApplyErrorCode]() -> std::expected<void, PulseForge::SceneError>
				{
					PulseForge::Project* const ActiveProject = m_Host.GetPrefabProject();
					PulseForge::Scene* const ActiveScene = m_Host.GetPrefabScene();
					if (!m_PendingReconcile || !ActiveProject || !ActiveScene ||
						ActiveProject->GetDescription().ID != ExpectedProjectID ||
						m_Host.GetPrefabSceneSessionGeneration() != ExpectedSceneSession)
						return std::unexpected(PulseForge::SceneError{ PulseForge::SceneErrorCode::InvalidPrefabInstance,
							"The pending prefab plan or project was superseded" });
					auto Result = PulseForge::PrefabAssetService::ApplyReconcile(
						m_PendingReconcile->GetSourceAsset(), ActiveProject->GetRootPath(), ActiveProject->GetAssetRegistry(),
						*ActiveScene, *m_PendingReconcile);
					if (!Result)
					{
						ApplyErrorCode = Result.error().Code;
						return std::unexpected(PulseForge::SceneError{
							PulseForge::SceneErrorCode::InvalidPrefabInstance, Result.error().Message });
					}
					return {};
				});
			if (!Applied)
			{
				if (ApplyErrorCode == PulseForge::PrefabAssetErrorCode::AssetNotFound ||
					ApplyErrorCode == PulseForge::PrefabAssetErrorCode::SourceFileMissing)
				{
					RecordMissingSourceConflict(RootID, ExpectedProjectID, ExpectedInstance, ExpectedSceneSession,
						"The prefab source became unavailable after planning; the materialized instance was preserved: " + Applied.error());
					return;
				}
				m_Host.ReportPrefabError("Linked prefab update failed: " + Applied.error());
				m_PendingReconcile.reset();
				m_PendingProjectID.reset();
				m_PendingSceneSessionGeneration.reset();
				return;
			}
			m_PendingReconcile.reset();
			m_PendingProjectID.reset();
			m_PendingSceneSessionGeneration.reset();
			m_Host.UpdatePrefabSceneDirty();
			m_Host.ReportPrefabStatus("Applied the planned linked prefab update. The scene remains unsaved.");
		});
		auto Status = GetStatus(Entity);
		if (!Status || !Status->has_value())
			return std::unexpected(Status ? "Linked instance is no longer available" : Status.error());
		m_Host.ReportPrefabStatus("Queued linked prefab update for the next authoring boundary.");
		return std::move(**Status);
	}

	std::expected<void, std::string> EditorPrefabInstanceController::ApplyOverrides(
		const PulseForge::Entity& Entity, bool All)
	{
		return QueueOverrideOperation(Entity, All, false);
	}

	std::expected<void, std::string> EditorPrefabInstanceController::RevertOverrides(
		const PulseForge::Entity& Entity, bool All)
	{
		return QueueOverrideOperation(Entity, All, true);
	}

	bool EditorPrefabInstanceController::IsLinkedPrefabEntity(const PulseForge::Entity& Entity) const
	{
		const PulseForge::Scene* const Scene = m_Host.GetPrefabScene();
		if (!Scene) return false;
		const auto Root = PulseForge::PrefabInstanceReconciler::FindLinkedRoot(*Scene, Entity);
		return Root && Root->has_value();
	}

	std::expected<void, std::string> EditorPrefabInstanceController::RecordPropertyOverride(
		const PulseForge::Entity& Entity, std::string_view ComponentID, std::string_view PropertyID)
	{
		PulseForge::Scene* const Scene = m_Host.GetPrefabScene();
		if (!Scene) return {};
		auto Root = PulseForge::PrefabInstanceReconciler::FindLinkedRoot(*Scene, Entity);
		if (!Root) return std::unexpected(Root.error().Message);
		if (!Root->has_value()) return {};
		auto Value = PulseForge::ReadProperty(Entity, ComponentID, PropertyID);
		if (!Value) return std::unexpected(Value.error().Message);
		auto Recorded = PulseForge::PrefabInstanceReconciler::RecordPropertyOverride(
			**Root, Entity, std::string(ComponentID), std::string(PropertyID), std::move(*Value));
		if (!Recorded) return std::unexpected(Recorded.error().Message);
		return {};
	}

	std::expected<void, std::string> EditorPrefabInstanceController::RecordRenameOverride(
		const PulseForge::Entity& Entity, std::string_view Name)
	{
		PulseForge::Scene* const Scene = m_Host.GetPrefabScene();
		if (!Scene) return {};
		auto Root = PulseForge::PrefabInstanceReconciler::FindLinkedRoot(*Scene, Entity);
		if (!Root) return std::unexpected(Root.error().Message);
		if (!Root->has_value()) return {};
		auto Recorded = PulseForge::PrefabInstanceReconciler::RecordRenameOverride(**Root, Entity, std::string(Name));
		if (!Recorded) return std::unexpected(Recorded.error().Message);
		return {};
	}

	std::expected<void, std::string> EditorPrefabInstanceController::ResetPropertyOverride(
		const PulseForge::Entity& Entity, std::string_view ComponentID, std::string_view PropertyID)
	{
		PulseForge::Project* const Project = m_Host.GetPrefabProject();
		PulseForge::Scene* const Scene = m_Host.GetPrefabScene();
		EditorPrefabDocumentEditor* const PrefabEditor = m_Host.GetPrefabDocumentEditor();
		if (!Project || !Scene || !PrefabEditor) return std::unexpected("Open the source project before resetting a prefab override");
		if (m_Host.IsPrefabRuntimeActive()) return std::unexpected("Stop Play before resetting a prefab override");
		auto Root = PulseForge::PrefabInstanceReconciler::FindLinkedRoot(*Scene, Entity);
		if (!Root) return std::unexpected(Root.error().Message);
		if (!Root->has_value()) return std::unexpected("Selected entity is not part of a linked prefab instance");
		auto Metadata = (**Root).GetPrefabInstance();
		if (!Metadata || !Metadata->has_value()) return std::unexpected("Linked prefab identity is unavailable");
		const auto Mapping = std::find_if((**Metadata).SourceEntities.begin(), (**Metadata).SourceEntities.end(),
			[&Entity](const PulseForge::PrefabEntityMapping& Item) { return Item.SceneEntity == Entity.GetUUID(); });
		if (Mapping == (**Metadata).SourceEntities.end())
			return std::unexpected("Local entity properties do not have a prefab source value to reset to");
		const PulseForge::UUID RootID = (**Root).GetUUID();
		const PulseForge::UUID EntityID = Entity.GetUUID();
		const PulseForge::UUID ExpectedProjectID = Project->GetDescription().ID;
		const PulseForge::PrefabInstanceComponent ExpectedInstance = **Metadata;
		const PulseForge::PrefabEntityAddress Address{ Mapping->SourceKey, false };
		const std::string Component(ComponentID);
		const std::string Property(PropertyID);
		m_Host.QueuePrefabOperation([this, RootID, EntityID, ExpectedProjectID, ExpectedInstance, Address, Component, Property]
		{
			PulseForge::Project* const ActiveProject = m_Host.GetPrefabProject();
			if (m_Host.IsPrefabRuntimeActive() || !ActiveProject || ActiveProject->GetDescription().ID != ExpectedProjectID ||
				!m_Host.GetPrefabScene() ||
				!m_Host.GetPrefabDocumentEditor())
			{
				m_Host.ReportPrefabWarning("The prefab override reset was cancelled because the scene or project is unavailable.");
				return;
			}
			PulseForge::Scene* const CurrentScene = m_Host.GetPrefabScene();
			const auto CurrentRoot = CurrentScene->FindEntity(RootID);
			const auto CurrentEntity = CurrentScene->FindEntity(EntityID);
			const auto CurrentMetadata = CurrentRoot ? CurrentRoot->GetPrefabInstance() :
				std::expected<std::optional<PulseForge::PrefabInstanceComponent>, PulseForge::SceneError>{ std::unexpected(
					PulseForge::SceneError{ PulseForge::SceneErrorCode::InvalidPrefabInstance, "Linked root is unavailable" }) };
			if (!CurrentRoot || !CurrentEntity || !CurrentMetadata || !CurrentMetadata->has_value() ||
				**CurrentMetadata != ExpectedInstance)
			{
				m_Host.ReportPrefabWarning("The linked prefab instance changed before its override reset; select the property and retry.");
				return;
			}
			if (!m_Host.EnsurePrefabDocumentOpen(ExpectedInstance.SourceAsset)) return;
			const PulseForgeEditor::EditorDocumentIdentity Identity{
				m_Host.GetPrefabProject()->GetDescription().ID, ExpectedInstance.SourceAsset, EditorDocumentKind::Prefab };
			auto* Session = m_Host.GetPrefabDocumentManager().Find(Identity);
			if (!Session || !Session->EditorState)
			{
				m_Host.ReportPrefabError("The prefab source draft is unavailable for override reset.");
				return;
			}
			auto Reset = m_Host.RunPrefabSceneEdit("Reset Prefab Override",
				[this, Session, RootID, EntityID, ExpectedInstance, Address, Component, Property]()
					-> std::expected<void, PulseForge::SceneError>
				{
					PulseForge::Scene* const ActiveScene = m_Host.GetPrefabScene();
					EditorPrefabDocumentEditor* const ActivePrefabEditor = m_Host.GetPrefabDocumentEditor();
					if (!ActiveScene || !ActivePrefabEditor)
						return std::unexpected(PulseForge::SceneError{
							PulseForge::SceneErrorCode::InvalidPrefabInstance, "Scene or prefab document editor was superseded" });
					const auto RootNow = ActiveScene->FindEntity(RootID);
					const auto EntityNow = ActiveScene->FindEntity(EntityID);
					const auto MetadataNow = RootNow ? RootNow->GetPrefabInstance() :
						std::expected<std::optional<PulseForge::PrefabInstanceComponent>, PulseForge::SceneError>{ std::unexpected(
							PulseForge::SceneError{ PulseForge::SceneErrorCode::InvalidPrefabInstance, "Linked root is unavailable" }) };
					if (!RootNow || !EntityNow || !MetadataNow || !MetadataNow->has_value() || **MetadataNow != ExpectedInstance)
						return std::unexpected(PulseForge::SceneError{
							PulseForge::SceneErrorCode::InvalidPrefabInstance, "Linked instance changed before override reset" });
					auto ResetResult = ActivePrefabEditor->ResetInstancePropertyOverride(
						*Session, *RootNow, Address, Component, Property);
					if (!ResetResult)
						return std::unexpected(PulseForge::SceneError{
							PulseForge::SceneErrorCode::InvalidPrefabInstance, ResetResult.error() });
					return {};
				});
			if (!Reset)
			{
				m_Host.ReportPrefabError("Prefab override reset failed: " + Reset.error());
				return;
			}
			m_Host.UpdatePrefabSceneDirty();
			m_Host.ReportPrefabStatus("Reset the selected prefab override to the current source draft. The scene remains unsaved.");
		});
		return {};
	}

	std::expected<void, std::string> EditorPrefabInstanceController::RecordReparent(
		const PulseForge::Entity& Entity, const std::optional<PulseForge::Entity>& Parent)
	{
		PulseForge::Scene* const Scene = m_Host.GetPrefabScene();
		if (!Scene) return {};
		auto Root = PulseForge::PrefabInstanceReconciler::FindLinkedRoot(*Scene, Entity);
		if (!Root) return std::unexpected(Root.error().Message);
		if (!Root->has_value() || **Root == Entity) return {};
		auto Recorded = PulseForge::PrefabInstanceReconciler::RecordReparentOverride(**Root, Entity, Parent);
		if (!Recorded) return std::unexpected(Recorded.error().Message);
		return {};
	}

	std::expected<void, std::string> EditorPrefabInstanceController::RecordRemoval(const PulseForge::Entity& Entity)
	{
		PulseForge::Scene* const Scene = m_Host.GetPrefabScene();
		if (!Scene) return {};
		auto Root = PulseForge::PrefabInstanceReconciler::FindLinkedRoot(*Scene, Entity);
		if (!Root) return std::unexpected(Root.error().Message);
		if (!Root->has_value() || **Root == Entity) return {};
		auto Recorded = PulseForge::PrefabInstanceReconciler::RecordRemoveSourceEntityOverride(**Root, Entity);
		if (!Recorded) return std::unexpected(Recorded.error().Message);
		return {};
	}

	std::expected<void, std::string> EditorPrefabInstanceController::QueueOverrideOperation(
		const PulseForge::Entity& Entity, bool All, bool Revert)
	{
		PulseForge::Project* const Project = m_Host.GetPrefabProject();
		PulseForge::Scene* const Scene = m_Host.GetPrefabScene();
		if (!Project || !Scene || !m_Host.GetPrefabDocumentEditor())
			return std::unexpected("Open a project and scene before changing linked prefab overrides");
		if (m_Host.IsPrefabRuntimeActive()) return std::unexpected("Stop Play before applying or reverting prefab overrides");
		auto Root = PulseForge::PrefabInstanceReconciler::FindLinkedRoot(*Scene, Entity);
		if (!Root) return std::unexpected(Root.error().Message);
		if (!Root->has_value()) return std::unexpected("The selected entity is not part of a linked prefab instance");
		const auto Metadata = (**Root).GetPrefabInstance();
		if (!Metadata || !Metadata->has_value()) return std::unexpected("Linked prefab metadata is unavailable");
		const PulseForge::UUID RootID = (**Root).GetUUID();
		const PulseForge::UUID SelectedID = Entity.GetUUID();
		const PulseForge::UUID ExpectedProjectID = Project->GetDescription().ID;
		const PulseForge::PrefabInstanceComponent ExpectedInstance = **Metadata;
		std::optional<PulseForge::PrefabEntityAddress> Address;
		if (!All)
		{
			const auto SourceMapping = std::find_if(ExpectedInstance.SourceEntities.begin(), ExpectedInstance.SourceEntities.end(),
				[SelectedID](const auto& Mapping) { return Mapping.SceneEntity == SelectedID; });
			if (SourceMapping != ExpectedInstance.SourceEntities.end())
				Address = PulseForge::PrefabEntityAddress{ SourceMapping->SourceKey, false };
			else
			{
				const auto LocalMapping = std::find_if(ExpectedInstance.LocalEntities.begin(), ExpectedInstance.LocalEntities.end(),
					[SelectedID](const auto& Mapping) { return Mapping.SceneEntity == SelectedID; });
				if (LocalMapping == ExpectedInstance.LocalEntities.end())
					return std::unexpected("The selected entity has no identity in the linked prefab instance");
				if (Revert) return std::unexpected("Instance-local entities have no source-backed overrides to revert");
				Address = PulseForge::PrefabEntityAddress{ LocalMapping->LocalKey, true };
			}
		}

		m_Host.QueuePrefabOperation([this, RootID, ExpectedProjectID, ExpectedInstance, Address, Revert]
		{
			if (m_Host.IsPrefabRuntimeActive())
			{
				m_Host.ReportPrefabWarning("Stop Play before applying or reverting prefab overrides.");
				return;
			}
			PulseForge::Scene* const Scene = m_Host.GetPrefabScene();
			PulseForge::Project* const Project = m_Host.GetPrefabProject();
			EditorPrefabDocumentEditor* const PrefabEditor = m_Host.GetPrefabDocumentEditor();
			if (!Project || Project->GetDescription().ID != ExpectedProjectID)
			{
				m_Host.ReportPrefabWarning("The prefab override operation was cancelled because the project changed.");
				return;
			}
			const auto RootNow = Scene ? Scene->FindEntity(RootID) : std::nullopt;
			if (!RootNow)
			{
				m_Host.ReportPrefabWarning("The linked prefab root disappeared before the operation could run.");
				return;
			}
			const auto MetadataNow = RootNow->GetPrefabInstance();
			if (!MetadataNow || !MetadataNow->has_value() || **MetadataNow != ExpectedInstance)
			{
				m_Host.ReportPrefabWarning("The linked instance changed before the operation could run; review and retry.");
				return;
			}
			if (!Project || !PrefabEditor || !m_Host.EnsurePrefabDocumentOpen(ExpectedInstance.SourceAsset)) return;
			const PulseForgeEditor::EditorDocumentIdentity Identity{
				Project->GetDescription().ID, ExpectedInstance.SourceAsset, EditorDocumentKind::Prefab };
			auto* Session = m_Host.GetPrefabDocumentManager().Find(Identity);
			if (!Session || !Session->EditorState)
			{
				m_Host.ReportPrefabError("The prefab document could not provide its owned source draft.");
				return;
			}

			if (!Revert)
			{
				auto Applied = PrefabEditor->ApplyInstanceOverridesToSourceDraft(*Session, ExpectedInstance, Address);
				if (!Applied)
				{
					m_Host.ReportPrefabError("Could not apply instance overrides to the prefab source draft: " + Applied.error());
					return;
				}
				m_Host.ReportPrefabStatus("Applied " + std::to_string(Applied->Applied) +
					" source-bound override(s) to the prefab draft. Save the prefab document to write them; instance intent remains recorded.");
				return;
			}

			if (!Scene) return;
			PulseForge::PrefabOverrideEditSummary Reverted;
			auto Recorded = m_Host.RunPrefabSceneEdit("Revert Linked Prefab Overrides",
				[this, Session, RootID, ExpectedInstance, Address, &Reverted]() -> std::expected<void, PulseForge::SceneError>
				{
					PulseForge::Scene* const ActiveScene = m_Host.GetPrefabScene();
					EditorPrefabDocumentEditor* const ActivePrefabEditor = m_Host.GetPrefabDocumentEditor();
					if (!ActiveScene || !ActivePrefabEditor)
						return std::unexpected(PulseForge::SceneError{ PulseForge::SceneErrorCode::InvalidPrefabInstance,
							"The scene or prefab document editor is unavailable" });
					const auto CurrentRoot = ActiveScene->FindEntity(RootID);
					if (!CurrentRoot) return std::unexpected(PulseForge::SceneError{
						PulseForge::SceneErrorCode::InvalidPrefabInstance, "The linked root is unavailable" });
					auto CurrentMetadata = CurrentRoot->GetPrefabInstance();
					if (!CurrentMetadata || !CurrentMetadata->has_value() || **CurrentMetadata != ExpectedInstance)
						return std::unexpected(PulseForge::SceneError{
							PulseForge::SceneErrorCode::InvalidPrefabInstance, "The linked instance changed before revert" });
					auto Result = ActivePrefabEditor->RevertInstanceOverrides(*Session, *ActiveScene, *CurrentRoot, Address);
					if (!Result) return std::unexpected(PulseForge::SceneError{
						PulseForge::SceneErrorCode::InvalidPrefabInstance, Result.error() });
					Reverted = *Result;
					return {};
				});
			if (!Recorded)
			{
				m_Host.ReportPrefabError("Could not revert linked-instance overrides: " + Recorded.error());
				return;
			}
			m_Host.UpdatePrefabSceneDirty();
			m_Host.ReportPrefabStatus("Reverted " + std::to_string(Reverted.Applied) +
				" linked-instance override(s) to the current prefab source draft. The scene remains unsaved.");
		});
		return {};
	}
} // namespace PulseForgeEditor
