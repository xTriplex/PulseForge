#include "Editor/EditorPrefabDocumentEditor.h"

#include "Assets/AssetTypes.h"
#include "Assets/PrefabAssetService.h"
#include "Assets/PrefabInstanceReconciler.h"
#include "Assets/PrefabSerializer.h"
#include "Editor/EditorAssetSelector.h"
#include "Editor/EditorHierarchyPanel.h"
#include "Editor/EditorInspectorPanel.h"
#include "Editor/EditorInteractionPolicy.h"
#include "Editor/EditorSceneEditController.h"
#include "Editor/EditorSceneEntityCommands.h"
#include "Editor/EditorSceneVisibilityCommands.h"
#include "Editor/EditorSceneWorkspaceState.h"
#include "Editor/EditorStyle.h"
#include "Scene/SceneSerializer.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace PulseForgeEditor
{
	namespace
	{
		struct LinkedPreview
		{
			std::unique_ptr<PulseForge::Scene> Scene;
			PulseForge::UUID Root;
		};

		[[nodiscard]] std::expected<LinkedPreview, std::string> CreateLinkedPreview(
			const PulseForge::PrefabSourceDocument& Source)
		{
			LinkedPreview Preview{ std::make_unique<PulseForge::Scene>(), {} };
			auto Linked = PulseForge::PrefabInstanceReconciler::InstantiateLinked(Source, *Preview.Scene);
			if (!Linked) return std::unexpected(Linked.error().Message);
			Preview.Root = Linked->Root.GetUUID();
			return std::move(Preview);
		}

		[[nodiscard]] std::optional<std::string> SerializeScene(const PulseForge::Scene& Scene)
		{
			auto Serialized = PulseForge::SceneSerializer::Serialize(Scene);
			if (!Serialized) return std::nullopt;
			return std::move(*Serialized);
		}

		[[nodiscard]] std::string MakeReconcileSummaryText(const PulseForge::PrefabReconcileSummary& Summary)
		{
			return "Added " + std::to_string(Summary.Added) + "  Existing processed " +
				std::to_string(Summary.ExistingEntitiesProcessed) + "  Removed " + std::to_string(Summary.Removed) +
				"  Conflicts " + std::to_string(Summary.ConflictCount);
		}
	}

	class PrefabDocumentState final : public EditorDocumentEditorState
	{
	public:
		PrefabDocumentState(
			PulseForge::PrefabSourceDocument SourceDocument,
			std::filesystem::path ProjectRootValue,
			const PulseForge::AssetRegistry& RegistryValue,
			std::filesystem::path ProjectRelativePathValue,
			std::string DisplayNameValue)
			: Source(std::move(SourceDocument)), ProjectRoot(std::move(ProjectRootValue)), Registry(&RegistryValue),
			  ProjectRelativePath(std::move(ProjectRelativePathValue)), DisplayName(std::move(DisplayNameValue))
		{
			History.Reset(&Source.GetScene());
			SourceWorkspace.Selection().SelectSingle(Source.GetRootKey());
		}

		PulseForge::PrefabSourceDocument Source;
		std::filesystem::path ProjectRoot;
		const PulseForge::AssetRegistry* Registry = nullptr;
		std::filesystem::path ProjectRelativePath;
		std::string DisplayName;
		std::unique_ptr<PulseForge::Scene> PreviewScene;
		PulseForge::UUID PreviewRoot;
		EditorSceneWorkspaceState SourceWorkspace;
		EditorSceneWorkspaceState PreviewWorkspace;
		EditorSceneEditController History;
		EditorHierarchyPanel HierarchyPanel;
		EditorInspectorPanel InspectorPanel;
		EditorAssetSelector AssetSelector;
		EditorSceneEntityCommands EntityCommands;
		EditorSceneVisibilityCommands VisibilityCommands;
		std::optional<PulseForge::PrefabReconcilePlan> PendingReconcile;
		std::optional<PulseForge::PrefabReconcileSummary> LastReconcileSummary;
		std::vector<PulseForge::PrefabConflictRecord> LastConflicts;
		std::string Status;
		std::string Error;
	};

	namespace
	{
		[[nodiscard]] PrefabDocumentState* GetState(EditorDocumentSession& Session)
		{
			return dynamic_cast<PrefabDocumentState*>(Session.EditorState.get());
		}

		[[nodiscard]] const PrefabDocumentState* GetState(const EditorDocumentSession& Session)
		{
			return dynamic_cast<const PrefabDocumentState*>(Session.EditorState.get());
		}

		[[nodiscard]] EditorSceneEditController::RestoreCallback MakeRestoreCallback(PrefabDocumentState& State)
		{
			return [&State](std::string_view Snapshot)
			{
				return State.History.Restore(Snapshot, State.Source.GetScene(), State.SourceWorkspace);
			};
		}

		void InvalidatePendingReconcile(PrefabDocumentState& State)
		{
			State.PendingReconcile.reset();
			State.LastReconcileSummary.reset();
			State.LastConflicts.clear();
		}

		void SynchronizeDocumentRevision(
			EditorDocumentManager& Documents,
			EditorDocumentSession& Session,
			PrefabDocumentState& State,
			const std::optional<std::string>& Before)
		{
			const auto After = SerializeScene(State.Source.GetScene());
			if (!Before || !After || *Before == *After) return;
			(void)Documents.MarkEdited(Session.Identity);
			InvalidatePendingReconcile(State);
			if (!State.History.IsDirty()) Session.SavedRevision = Session.Revision;
		}

		[[nodiscard]] std::expected<void, std::string> FinishHistoryEdit(
			PrefabDocumentState& State,
			EditorDocumentSession& Session)
		{
			const auto Before = SerializeScene(State.Source.GetScene());
			const auto Result = State.History.Finish(
				&State.Source.GetScene(), State.SourceWorkspace, MakeRestoreCallback(State));
			if (!Result) return std::unexpected(Result.error());
			const auto After = SerializeScene(State.Source.GetScene());
			if (Before && After && *Before != *After) InvalidatePendingReconcile(State);
			if (!State.History.IsDirty()) Session.SavedRevision = Session.Revision;
			return {};
		}

		class PrefabDocumentActions final : public EditorHierarchyPanelActionSink,
											 public EditorInspectorPanelActions,
											 public EditorSceneEntityCommandActions
		{
		public:
			PrefabDocumentActions(
				EditorDocumentManager& Documents,
				EditorDocumentSession& Session,
				PrefabDocumentState& State)
				: m_Documents(Documents), m_Session(Session), m_State(State)
			{
			}

			void HandleHierarchyPanelAction(const EditorHierarchyPanelAction& Action) override
			{
				auto* Scene = &m_State.Source.GetScene();
				switch (Action.Kind)
				{
				case EditorHierarchyPanelActionKind::CreateEntity:
				{
					const PulseForge::UUID ParentID = m_State.SourceWorkspace.Selection().GetPrimary().value_or(
						m_State.Source.GetRootKey());
					const auto Parent = Scene->FindEntity(ParentID);
					if (!Parent)
					{
						ReportEditorSceneCommandError("The prefab parent entity no longer exists.");
						break;
					}
					const auto Recorded = RunEditorSceneEdit("Create Prefab Entity",
						[Scene, Parent = *Parent, this]() -> std::expected<void, PulseForge::SceneError>
						{
							auto Created = Scene->CreateEntity();
							if (!Created) return std::unexpected(Created.error());
							if (auto Parented = Created->SetParent(Parent); !Parented) return std::unexpected(Parented.error());
							SelectSingleEditorEntity(Created->GetUUID());
							return {};
						});
					if (!Recorded) ReportEditorSceneCommandError("Prefab entity creation failed: " + Recorded.error());
					else SetEditorSceneCommandStatus("Created a child entity in the prefab source.");
					break;
				}
				case EditorHierarchyPanelActionKind::DuplicateSelection:
					if (m_State.SourceWorkspace.Selection().Contains(m_State.Source.GetRootKey()))
					{
						ReportEditorSceneCommandError("The prefab root cannot be duplicated inside its own source document.");
						break;
					}
					m_State.EntityCommands.DuplicateSelectedEntities(Scene, m_State.SourceWorkspace, *this);
					break;
				case EditorHierarchyPanelActionKind::DeleteSelection:
					if (m_State.SourceWorkspace.Selection().Contains(m_State.Source.GetRootKey()))
					{
						ReportEditorSceneCommandError("The prefab root cannot be deleted from its source document.");
						break;
					}
					m_State.EntityCommands.DeleteSelectedEntities(Scene, m_State.SourceWorkspace, *this);
					break;
				case EditorHierarchyPanelActionKind::CreatePrefab:
					SetEditorSceneCommandStatus("Nested prefab authoring is unavailable inside a prefab source document.");
					break;
				case EditorHierarchyPanelActionKind::ToggleVisibility:
					m_State.VisibilityCommands.ToggleEntityVisibility(
						Scene, m_State.SourceWorkspace, Action.Entity, false, *this);
					break;
				case EditorHierarchyPanelActionKind::MakeRoot:
					if (Action.Entity != m_State.Source.GetRootKey())
						ReportEditorSceneCommandError("Prefab source entities must remain under the stable prefab root.");
					break;
				case EditorHierarchyPanelActionKind::ParentUnderSelection:
					m_State.EntityCommands.ReparentEntity(Scene, Action.Entity, Action.Parent, *this);
					break;
				case EditorHierarchyPanelActionKind::ReportError:
					ReportEditorSceneCommandError(Action.Message);
					break;
				}
			}

			void FinishInspectorSceneEdit() override
			{
				if (auto Result = FinishHistoryEdit(m_State, m_Session); !Result) ReportInspectorError(Result.error());
			}

			[[nodiscard]] std::expected<void, std::string> RunInspectorSceneEdit(
				std::string Description,
				std::function<std::expected<void, PulseForge::SceneError>()> Operation) override
			{
				return RunEditorSceneEdit(std::move(Description), std::move(Operation));
			}

			void ReportInspectorError(std::string Message) override
			{
				m_State.Error = std::move(Message);
			}

			[[nodiscard]] std::expected<void, std::string> RunEditorSceneEdit(
				std::string Description,
				std::function<std::expected<void, PulseForge::SceneError>()> Operation) override
			{
				const auto Before = SerializeScene(m_State.Source.GetScene());
				const uint32_t ActiveItem = GImGui ? GImGui->ActiveId : 0;
				const auto Result = m_State.History.Run(
					&m_State.Source.GetScene(), m_State.SourceWorkspace, false, ActiveItem,
					[] { return GImGui ? GImGui->ActiveId : 0; }, std::move(Description), std::move(Operation),
					MakeRestoreCallback(m_State));
				SynchronizeDocumentRevision(m_Documents, m_Session, m_State, Before);
				if (!Result)
				{
					m_State.Error = Result.error();
					return std::unexpected(Result.error());
				}
				m_State.Error.clear();
				return {};
			}

			void ReportEditorSceneCommandError(std::string Message) override
			{
				m_State.Error = std::move(Message);
			}

			void SetEditorSceneCommandStatus(std::string Message) override
			{
				m_State.Status = std::move(Message);
			}

			void ReportEditorSceneCommandWarning(std::string Message) override
			{
				m_State.Error = std::move(Message);
			}

			void SetEditorSceneSelection(std::vector<PulseForge::UUID> Identifiers) override
			{
				const std::optional<PulseForge::UUID> Primary = Identifiers.empty()
					? std::nullopt
					: std::optional<PulseForge::UUID>{ Identifiers.back() };
				m_State.SourceWorkspace.Selection().Set(std::move(Identifiers), Primary);
			}

			void SelectSingleEditorEntity(std::optional<PulseForge::UUID> Identifier) override
			{
				if (Identifier) m_State.SourceWorkspace.Selection().SelectSingle(*Identifier);
				else m_State.SourceWorkspace.Selection().Clear();
			}

		private:
			EditorDocumentManager& m_Documents;
			EditorDocumentSession& m_Session;
			PrefabDocumentState& m_State;
		};
	}

	EditorPrefabDocumentEditor::EditorPrefabDocumentEditor(
		EditorDocumentManager& Documents,
		EditorDocumentEditorRegistry& Editors,
		EditorStyle& Style)
		: m_Documents(Documents), m_Editors(Editors), m_Style(Style)
	{
	}

	std::expected<void, std::string> EditorPrefabDocumentEditor::Register(EditorDocumentKind PrefabDocumentKind)
	{
		if (PrefabDocumentKind == EditorDocumentKind::Level)
			return std::unexpected("A prefab document cannot use the Level document kind");
		if (m_DocumentKind)
			return std::unexpected("The prefab document editor is already registered");

		EditorDocumentEditorAdapter Adapter;
		Adapter.Kind = PrefabDocumentKind;
		Adapter.IsEligible = [this](const EditorDocumentEditorContext& Context) { return IsEligible(Context); };
		Adapter.LoadSession = [this](const EditorDocumentEditorContext& Context) { return LoadSession(Context); };
		Adapter.DrawWorkspace = [this](EditorDocumentSession& Session) { DrawWorkspace(Session); };
		Adapter.Save = [this](EditorDocumentSession& Session, uint64_t SavedRevision)
		{ return SaveSession(Session, SavedRevision); };
		Adapter.Discard = [this](EditorDocumentSession& Session) { return DiscardSession(Session); };
		Adapter.FocusedCommand = [this](EditorDocumentSession& Session, EditorDocumentCommand Command)
		{ return HandleFocusedCommand(Session, Command); };
		Adapter.AssetLocationChanged = [this](EditorDocumentSession& Session, const auto& Path, std::string_view Name)
		{ return UpdateAssetLocation(Session, Path, Name); };
		Adapter.PrepareClose = [this](const EditorDocumentCloseContext& Context) { return PrepareClose(Context); };
		auto Registered = m_Editors.Register(std::move(Adapter));
		if (!Registered) return std::unexpected(Registered.error());
		m_DocumentKind = PrefabDocumentKind;
		return {};
	}

	bool EditorPrefabDocumentEditor::Open(EditorDocumentSession& Session, const EditorDocumentEditorContext& Context) const
	{
		return m_DocumentKind && Session.Identity.Kind == *m_DocumentKind && m_Editors.Load(Session, Context);
	}

	bool EditorPrefabDocumentEditor::Save(EditorDocumentSession& Session) const
	{
		return m_DocumentKind && Session.Identity.Kind == *m_DocumentKind && m_Editors.Save(Session);
	}

	bool EditorPrefabDocumentEditor::Discard(EditorDocumentSession& Session) const
	{
		return m_DocumentKind && Session.Identity.Kind == *m_DocumentKind && m_Editors.Discard(Session);
	}

	bool EditorPrefabDocumentEditor::DispatchFocusedCommand(
		EditorDocumentSession& Session,
		EditorDocumentCommand Command) const
	{
		return m_DocumentKind && Session.Identity.Kind == *m_DocumentKind && m_Editors.DispatchFocusedCommand(Session, Command);
	}

	bool EditorPrefabDocumentEditor::IsEligible(const EditorDocumentEditorContext& Context) const
	{
		if (!m_DocumentKind || Context.Identity.Kind != *m_DocumentKind || !Context.Registry || Context.ProjectRoot.empty()) return false;
		const auto Asset = Context.Registry->Find(Context.Identity.Asset);
		return Asset && Asset->ProjectRelativePath == Context.ProjectRelativePath &&
			PulseForge::HasAssetTypeExtension(Asset->ProjectRelativePath, PulseForge::AssetFileType::Prefab);
	}

	std::expected<std::unique_ptr<EditorDocumentEditorState>, std::string> EditorPrefabDocumentEditor::LoadSession(
		const EditorDocumentEditorContext& Context) const
	{
		if (!IsEligible(Context)) return std::unexpected("The document does not resolve to a managed prefab asset");
		auto Source = PulseForge::PrefabAssetService::LoadSource(Context.Identity.Asset, Context.ProjectRoot, *Context.Registry);
		if (!Source) return std::unexpected("Could not load prefab source: " + Source.error().Message);
		auto State = std::make_unique<PrefabDocumentState>(
			std::move(*Source), Context.ProjectRoot, *Context.Registry, Context.ProjectRelativePath, Context.DisplayName);
		auto Preview = CreateLinkedPreview(State->Source);
		if (!Preview) return std::unexpected("Could not create the isolated prefab preview: " + Preview.error());
		State->PreviewScene = std::move(Preview->Scene);
		State->PreviewRoot = Preview->Root;
		State->PreviewWorkspace.Selection().SelectSingle(State->PreviewRoot);
		return std::unique_ptr<EditorDocumentEditorState>(std::move(State));
	}

	void EditorPrefabDocumentEditor::DrawWorkspace(EditorDocumentSession& Session)
	{
		auto* State = GetState(Session);
		if (!State || !State->PreviewScene)
		{
			ImGui::TextUnformatted("The prefab document state is unavailable.");
			return;
		}

		const auto Root = State->Source.GetScene().FindEntity(State->Source.GetRootKey());
		std::string RootName = "<invalid root>";
		if (Root)
			if (const auto RootTag = Root->GetTag()) RootName = RootTag->Name;
		m_Style.IconText(EditorIcon::Prefab, RootName);
		ImGui::SameLine();
		m_Style.TextMuted(State->History.IsDirty() ? "Unsaved draft" : "Saved source");

		if (m_Style.BeginToolbar("##PrefabDocumentToolbar"))
		{
			ImGui::BeginDisabled(!State->History.CanUndo());
			if (m_Style.SmallButton(EditorIcon::Refresh, "Undo"))
				(void)HandleFocusedCommand(Session, EditorDocumentCommand::Undo);
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::BeginDisabled(!State->History.CanRedo());
			if (m_Style.SmallButton(EditorIcon::Refresh, "Redo"))
				(void)HandleFocusedCommand(Session, EditorDocumentCommand::Redo);
			ImGui::EndDisabled();
			ImGui::SameLine();
			if (m_Style.SmallButton(EditorIcon::Save, "Save"))
			{
				if (auto Finished = FinishHistoryEdit(*State, Session); !Finished) State->Error = Finished.error();
				else if (!Save(Session)) State->Error = State->Error.empty() ? "Could not save the prefab source." : State->Error;
			}
			ImGui::SameLine();
			ImGui::BeginDisabled(!Session.IsDirty());
			if (m_Style.SmallButton(EditorIcon::Clear, "Discard"))
			{
				if (!Discard(Session)) State->Error = State->Error.empty() ? "Could not reload the saved prefab source." : State->Error;
			}
			ImGui::EndDisabled();
		}
		m_Style.EndToolbar();

		ImGui::Text("Source entities  %zu    Preview entities  %zu", State->Source.GetScene().GetEntityCount(),
			State->PreviewScene->GetEntityCount());
		ImGui::Text("Source UUID  %s", State->Source.GetRootKey().ToString().c_str());

		PrefabDocumentActions Actions(m_Documents, Session, *State);
		if (ImGui::BeginTable("##PrefabDocumentPanels", 2,
				ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp))
		{
			ImGui::TableSetupColumn("Hierarchy", ImGuiTableColumnFlags_WidthStretch, 0.42f);
			ImGui::TableSetupColumn("Inspector", ImGuiTableColumnFlags_WidthStretch, 0.58f);
			ImGui::TableNextColumn();
			if (ImGui::BeginChild("##PrefabSourceHierarchy", ImVec2(0.0f, 390.0f), ImGuiChildFlags_None))
				State->HierarchyPanel.DrawContents(
					&State->Source.GetScene(), State->SourceWorkspace, m_Style, Actions);
			ImGui::EndChild();
			ImGui::TableNextColumn();
			if (ImGui::BeginChild("##PrefabSourceInspector", ImVec2(0.0f, 390.0f), ImGuiChildFlags_None))
			{
				const bool FinishCurrentEdit = ShouldFinishSceneEditForInspector(
					State->History.IsActive(), State->History.InspectorHistoryItem(), GImGui ? GImGui->ActiveId : 0);
				const std::vector<PulseForge::AssetRecord> Assets = State->Registry ? State->Registry->GetAssets() :
					std::vector<PulseForge::AssetRecord>{};
				State->InspectorPanel.DrawContents(&State->Source.GetScene(), State->SourceWorkspace, m_Style, State->AssetSelector,
					Assets, State->Registry != nullptr, FinishCurrentEdit, Actions);
			}
			ImGui::EndChild();
			ImGui::EndTable();
		}

		if (m_Style.SectionHeader(EditorIcon::Cube, "Linked Preview Reconcile", true))
		{
			ImGui::TextWrapped("Preview changes are isolated to this document. Plan an update, review conflicts, then apply it explicitly.");
			if (m_Style.SmallButton(EditorIcon::Refresh, "Plan Preview Update"))
			{
				auto Result = ExecuteIntent(Session, EditorPrefabDocumentIntent::PlanPreviewReconcile);
				if (!Result) State->Error = Result.error();
			}
			EditorPrefabDocumentSummary Summary;
			if (auto CurrentSummary = GetSummary(Session)) Summary = std::move(*CurrentSummary);
			if (Summary.HasPendingReconcile)
			{
				ImGui::TextWrapped("%s", MakeReconcileSummaryText(Summary.Reconcile).c_str());
				if (Summary.Conflicts.empty())
					m_Style.TextMuted("No conflicts were found.");
				else
				{
					for (const PulseForge::PrefabConflictRecord& Conflict : Summary.Conflicts)
						ImGui::BulletText("%s", Conflict.Message.c_str());
				}
				ImGui::BeginDisabled(!Summary.CanApplyReconcile && Summary.Conflicts.empty());
				if (m_Style.AccentButton(EditorIcon::Refresh,
					Summary.CanApplyReconcile ? "Apply Preview Update" : "Record Conflicts"))
				{
					auto Result = ExecuteIntent(Session, EditorPrefabDocumentIntent::ApplyPreviewReconcile);
					if (!Result) State->Error = Result.error();
				}
				ImGui::EndDisabled();
			}
			else if (Summary.Reconcile.Added + Summary.Reconcile.ExistingEntitiesProcessed + Summary.Reconcile.Removed > 0)
				m_Style.TextMuted(("Last preview update: " + MakeReconcileSummaryText(Summary.Reconcile)).c_str());
			if (!Summary.HasPendingReconcile && !Summary.Conflicts.empty())
				for (const PulseForge::PrefabConflictRecord& Conflict : Summary.Conflicts)
					ImGui::BulletText("%s", Conflict.Message.c_str());

			ImGui::Separator();
			if (ImGui::BeginChild("##PrefabLinkedPreview", ImVec2(0.0f, 150.0f), ImGuiChildFlags_Borders))
			{
				for (const PulseForge::Entity& Entity : State->PreviewScene->GetEntities())
				{
					const auto Tag = Entity.GetTag();
					const std::string Label = Tag ? Tag->Name : "<unavailable entity>";
					ImGui::Text("%s  (%s)", Label.c_str(), Entity.GetUUID().ToString().c_str());
				}
			}
			ImGui::EndChild();
		}

		if (!State->Error.empty())
			ImGui::TextWrapped("Error: %s", State->Error.c_str());
		else if (!State->Status.empty())
			m_Style.TextMuted(State->Status);

		if (State->History.IsActive() && !ImGui::IsAnyItemActive())
			if (auto Finished = FinishHistoryEdit(*State, Session); !Finished) State->Error = Finished.error();
	}

	bool EditorPrefabDocumentEditor::SaveSession(EditorDocumentSession& Session, uint64_t SavedRevision) const
	{
		(void)SavedRevision;
		auto* State = GetState(Session);
		if (!State || !State->Registry || State->Source.GetSourceAsset() != Session.Identity.Asset)
			return false;
		if (auto Finished = FinishHistoryEdit(*State, Session); !Finished)
		{
			State->Error = Finished.error();
			return false;
		}
		auto Saved = PulseForge::PrefabAssetService::SaveSource(
			Session.Identity.Asset, State->ProjectRoot, *State->Registry, State->Source);
		if (!Saved)
		{
			State->Error = "Prefab source save failed: " + Saved.error().Message;
			return false;
		}
		State->History.MarkSaved(&State->Source.GetScene());
		State->PendingReconcile.reset();
		State->LastReconcileSummary.reset();
		State->LastConflicts.clear();
		State->Error.clear();
		State->Status = "Saved prefab source. Plan a preview update to review the saved source against its linked preview.";
		return true;
	}

	bool EditorPrefabDocumentEditor::DiscardSession(EditorDocumentSession& Session) const
	{
		auto* State = GetState(Session);
		if (!State || !State->Registry) return false;
		auto Source = PulseForge::PrefabAssetService::LoadSource(Session.Identity.Asset, State->ProjectRoot, *State->Registry);
		if (!Source)
		{
			State->Error = "Could not reload prefab source: " + Source.error().Message;
			return false;
		}
		auto Preview = CreateLinkedPreview(*Source);
		if (!Preview)
		{
			State->Error = "Could not rebuild the isolated prefab preview: " + Preview.error();
			return false;
		}

		State->Source = std::move(*Source);
		State->PreviewScene = std::move(Preview->Scene);
		State->PreviewRoot = Preview->Root;
		State->SourceWorkspace.Reset();
		State->SourceWorkspace.Selection().SelectSingle(State->Source.GetRootKey());
		State->PreviewWorkspace.Reset();
		State->PreviewWorkspace.Selection().SelectSingle(State->PreviewRoot);
		State->History.Reset(&State->Source.GetScene());
		State->PendingReconcile.reset();
		State->LastReconcileSummary.reset();
		State->LastConflicts.clear();
		State->Error.clear();
		State->Status = "Reloaded the saved prefab source and reset its draft history.";
		return true;
	}

	bool EditorPrefabDocumentEditor::HandleFocusedCommand(
		EditorDocumentSession& Session,
		EditorDocumentCommand Command) const
	{
		auto* State = GetState(Session);
		if (!State) return false;
		if (auto Finished = FinishHistoryEdit(*State, Session); !Finished)
		{
			State->Error = Finished.error();
			return false;
		}
		const auto Before = SerializeScene(State->Source.GetScene());
		const auto Restore = MakeRestoreCallback(*State);
		auto Result = Command == EditorDocumentCommand::Undo
			? State->History.Undo(&State->Source.GetScene(), State->SourceWorkspace, false, Restore)
			: State->History.Redo(&State->Source.GetScene(), State->SourceWorkspace, false, Restore);
		if (!Result)
		{
			State->Error = Result.error();
			return false;
		}
		if (!*Result) return false;
		SynchronizeDocumentRevision(m_Documents, Session, *State, Before);
		if (!State->History.IsDirty()) Session.SavedRevision = Session.Revision;
		State->Error.clear();
		State->Status = Command == EditorDocumentCommand::Undo ? "Undid the last prefab source edit." : "Redid the last prefab source edit.";
		return true;
	}

	bool EditorPrefabDocumentEditor::UpdateAssetLocation(
		EditorDocumentSession& Session,
		const std::filesystem::path& ProjectRelativePath,
		std::string_view DisplayName) const
	{
		auto* State = GetState(Session);
		if (!State || !PulseForge::HasAssetTypeExtension(ProjectRelativePath, PulseForge::AssetFileType::Prefab)) return false;
		State->ProjectRelativePath = ProjectRelativePath;
		State->DisplayName.assign(DisplayName);
		return true;
	}

	std::expected<void, std::string> EditorPrefabDocumentEditor::PrepareClose(const EditorDocumentCloseContext& Context) const
	{
		const auto* State = dynamic_cast<const PrefabDocumentState*>(Context.EditorState);
		if (!State || State->Source.GetSourceAsset() != Context.Identity.Asset)
			return std::unexpected("The prefab document editor state is unavailable");
		return {};
	}

	std::expected<EditorPrefabDocumentOperationResult, std::string> EditorPrefabDocumentEditor::ExecuteIntent(
		EditorDocumentSession& Session,
		EditorPrefabDocumentIntent Intent)
	{
		auto* State = GetState(Session);
		if (!State || Session.Identity.Asset != State->Source.GetSourceAsset())
			return std::unexpected("The prefab document session is unavailable");

		if (Intent == EditorPrefabDocumentIntent::PlanPreviewReconcile)
		{
			if (State->History.IsDirty())
				return std::unexpected("Save the prefab source draft before planning a preview update.");
			State->PendingReconcile.reset();
			State->LastReconcileSummary.reset();
			State->LastConflicts.clear();
			const auto LinkedRoot = State->PreviewScene ? State->PreviewScene->FindEntity(State->PreviewRoot) : std::nullopt;
			if (!LinkedRoot) return std::unexpected("The linked preview root is unavailable");
			if (!State->Registry) return std::unexpected("The prefab project registry is unavailable");
			auto Plan = PulseForge::PrefabAssetService::PlanReconcile(State->Source.GetSourceAsset(), State->ProjectRoot,
				*State->Registry, *State->PreviewScene, *LinkedRoot);
			if (!Plan)
			{
				State->Error = Plan.error().Message;
				State->LastConflicts = Plan.error().Conflicts;
				return std::unexpected(Plan.error().Message);
			}
			EditorPrefabDocumentOperationResult Result;
			Result.Intent = Intent;
			Result.Reconcile = Plan->GetSummary();
			Result.Conflicts = Plan->GetConflicts();
			Result.CanApply = Plan->CanApply();
			State->LastReconcileSummary = Result.Reconcile;
			State->LastConflicts = Result.Conflicts;
			State->PendingReconcile = std::move(*Plan);
			State->Error.clear();
			State->Status = Result.CanApply ? "Preview update is ready for explicit application." :
				"Preview update has conflicts and cannot be applied.";
			return Result;
		}

		if (!State->PendingReconcile)
			return std::unexpected("Plan a preview reconcile before applying it");
		if (!State->Registry) return std::unexpected("The prefab project registry is unavailable");
		auto Applied = PulseForge::PrefabAssetService::ApplyReconcile(State->Source.GetSourceAsset(), State->ProjectRoot,
			*State->Registry, *State->PreviewScene, *State->PendingReconcile);
		if (!Applied)
		{
			State->Error = Applied.error().Message;
			State->LastConflicts = Applied.error().Conflicts;
			State->PendingReconcile.reset();
			return std::unexpected(Applied.error().Message);
		}
		EditorPrefabDocumentOperationResult Result;
		Result.Intent = Intent;
		Result.Reconcile = *Applied;
		Result.Changed = Applied->Added > 0 || Applied->ExistingEntitiesProcessed > 0 || Applied->Removed > 0 ||
			Applied->ConflictCount > 0;
		State->LastReconcileSummary = *Applied;
		State->LastConflicts.clear();
		State->PendingReconcile.reset();
		State->Error.clear();
		State->Status = "Applied the planned update to the isolated prefab preview.";
		return Result;
	}

	std::expected<void, std::string> EditorPrefabDocumentEditor::RecordPreviewOverride(
		EditorDocumentSession& Session,
		const PulseForge::PrefabOverrideRecord& Override)
	{
		auto* State = GetState(Session);
		const auto Root = State && State->PreviewScene ? State->PreviewScene->FindEntity(State->PreviewRoot) : std::nullopt;
		if (!State || !Root) return std::unexpected("The linked prefab preview is unavailable");
		auto Recorded = PulseForge::PrefabInstanceReconciler::RecordOverride(*Root, Override);
		if (!Recorded)
		{
			State->Error = Recorded.error().Message;
			return std::unexpected(Recorded.error().Message);
		}
		InvalidatePendingReconcile(*State);
		State->Error.clear();
		State->Status = "Recorded an explicit preview override.";
		return {};
	}

	std::expected<void, std::string> EditorPrefabDocumentEditor::ResetPreviewOverride(
		EditorDocumentSession& Session,
		PulseForge::PrefabEntityAddress EntityAddress,
		std::string_view ComponentID,
		std::string_view PropertyID)
	{
		auto* State = GetState(Session);
		const auto Root = State && State->PreviewScene ? State->PreviewScene->FindEntity(State->PreviewRoot) : std::nullopt;
		if (!State || !Root) return std::unexpected("The linked prefab preview is unavailable");
		auto Reset = PulseForge::PrefabInstanceReconciler::ResetOverride(
			*Root, State->Source, EntityAddress, ComponentID, PropertyID);
		if (!Reset)
		{
			State->Error = Reset.error().Message;
			return std::unexpected(Reset.error().Message);
		}
		InvalidatePendingReconcile(*State);
		State->Error.clear();
		State->Status = "Reset the selected preview override.";
		return {};
	}

	std::expected<PulseForge::PrefabOverrideEditSummary, std::string>
	EditorPrefabDocumentEditor::ApplyInstanceOverridesToSourceDraft(
		EditorDocumentSession& Session,
		const PulseForge::PrefabInstanceComponent& Instance,
		std::optional<PulseForge::PrefabEntityAddress> OnlyEntity)
	{
		auto* State = GetState(Session);
		if (!State || Session.Identity.Asset != State->Source.GetSourceAsset())
			return std::unexpected("The prefab source draft is unavailable");
		if (auto Finished = FinishHistoryEdit(*State, Session); !Finished)
			return std::unexpected(Finished.error());

		const auto Before = SerializeScene(State->Source.GetScene());
		if (!Before) return std::unexpected("Could not snapshot the prefab source draft before applying instance overrides");
		PulseForge::PrefabOverrideEditSummary Summary;
		const auto Applied = State->History.Run(
			&State->Source.GetScene(), State->SourceWorkspace, false, 0, [] { return 0; },
			"Apply Linked Instance Overrides to Prefab Draft",
			[State, &Instance, OnlyEntity, &Summary]() -> std::expected<void, PulseForge::SceneError>
			{
				auto Result = PulseForge::PrefabInstanceReconciler::ApplyOverridesToSource(State->Source, Instance, OnlyEntity);
				if (!Result)
					return std::unexpected(PulseForge::SceneError{
						PulseForge::SceneErrorCode::InvalidPrefabInstance, Result.error().Message });
				Summary = *Result;
				return {};
			}, MakeRestoreCallback(*State));
		SynchronizeDocumentRevision(m_Documents, Session, *State, Before);
		if (!Applied)
		{
			State->Error = Applied.error();
			return std::unexpected(Applied.error());
		}

		State->Error.clear();
		State->Status = Summary.Applied == 0
			? "No selected source-bound overrides were applied; instance-local edits remain on the scene."
			: "Applied instance overrides to the prefab source draft. Save this prefab document to write them to disk; instance intent remains recorded.";
		return Summary;
	}

	std::expected<PulseForge::PrefabOverrideEditSummary, std::string> EditorPrefabDocumentEditor::RevertInstanceOverrides(
		EditorDocumentSession& Session,
		PulseForge::Scene& Destination,
		const PulseForge::Entity& LinkedRoot,
		std::optional<PulseForge::PrefabEntityAddress> OnlyEntity)
	{
		auto* State = GetState(Session);
		if (!State || Session.Identity.Asset != State->Source.GetSourceAsset())
			return std::unexpected("The prefab source document is unavailable for this instance");
		if (auto Finished = FinishHistoryEdit(*State, Session); !Finished)
			return std::unexpected(Finished.error());
		auto Reverted = PulseForge::PrefabInstanceReconciler::RevertOverrides(
			Destination, LinkedRoot, State->Source, OnlyEntity);
		if (!Reverted)
		{
			State->Error = Reverted.error().Message;
			return std::unexpected(Reverted.error().Message);
		}
		State->Error.clear();
		State->Status = Reverted->Applied == 0
			? "No selected mapped overrides were available to revert."
			: "Reverted the selected instance overrides to the current prefab source draft. The scene remains unsaved.";
		return *Reverted;
	}

	std::expected<void, std::string> EditorPrefabDocumentEditor::ResetInstancePropertyOverride(
		EditorDocumentSession& Session,
		const PulseForge::Entity& LinkedRoot,
		PulseForge::PrefabEntityAddress Address,
		std::string_view ComponentID,
		std::string_view PropertyID)
	{
		auto* State = GetState(Session);
		if (!State || Session.Identity.Asset != State->Source.GetSourceAsset())
			return std::unexpected("The prefab source document is unavailable for this instance");
		if (auto Finished = FinishHistoryEdit(*State, Session); !Finished)
			return std::unexpected(Finished.error());
		auto Reset = PulseForge::PrefabInstanceReconciler::ResetOverride(
			LinkedRoot, State->Source, Address, ComponentID, PropertyID);
		if (!Reset)
		{
			State->Error = Reset.error().Message;
			return std::unexpected(Reset.error().Message);
		}
		State->Error.clear();
		return {};
	}

	std::expected<EditorPrefabDocumentSummary, std::string> EditorPrefabDocumentEditor::GetSummary(
		const EditorDocumentSession& Session) const
	{
		const auto* State = GetState(Session);
		if (!State || Session.Identity.Asset != State->Source.GetSourceAsset())
			return std::unexpected("The prefab document session is unavailable");
		EditorPrefabDocumentSummary Summary;
		Summary.Asset = State->Source.GetSourceAsset();
		Summary.SourceRoot = State->Source.GetRootKey();
		const std::string_view Fingerprint = State->Source.GetFingerprint();
		Summary.SourceFingerprint.assign(Fingerprint.data(), Fingerprint.size());
		Summary.SourceEntityCount = State->Source.GetScene().GetEntityCount();
		Summary.PreviewEntityCount = State->PreviewScene ? State->PreviewScene->GetEntityCount() : 0;
		Summary.DraftDirty = State->History.IsDirty();
		Summary.CanUndo = State->History.CanUndo();
		Summary.CanRedo = State->History.CanRedo();
		Summary.HasPendingReconcile = State->PendingReconcile.has_value();
		if (State->PendingReconcile)
		{
			Summary.Reconcile = State->PendingReconcile->GetSummary();
			Summary.Conflicts = State->PendingReconcile->GetConflicts();
			Summary.CanApplyReconcile = State->PendingReconcile->CanApply();
		}
		else if (State->LastReconcileSummary)
		{
			Summary.Reconcile = *State->LastReconcileSummary;
		}
		Summary.Conflicts = State->LastConflicts;
		Summary.Status = State->Status;
		Summary.Error = State->Error;
		return Summary;
	}
} // namespace PulseForgeEditor
