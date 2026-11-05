#include "Editor/EditorMaterialDocumentEditor.h"

#include "Assets/AssetTypes.h"
#include "Assets/Project.h"
#include "Editor/EditorAssetSelector.h"
#include "Editor/EditorMaterialDetailsPanel.h"
#include "Editor/EditorMaterialDraftController.h"
#include "Editor/EditorStyle.h"

#include <imgui.h>

#include <algorithm>
#include <memory>
#include <utility>

namespace PulseForgeEditor
{
	struct EditorMaterialDocumentEditor::State final : EditorDocumentEditorState
	{
		std::string Error;
	};

	EditorMaterialDocumentEditor::EditorMaterialDocumentEditor(EditorDocumentManager& Documents,
		EditorDocumentEditorRegistry& Editors, EditorMaterialDraftController& Drafts, EditorMaterialDetailsPanel& Panel,
		EditorAssetSelector& AssetSelector, EditorStyle& Style, EditorMaterialDetailsActionSink& Actions,
		ProjectProvider Project, AssetListProvider Assets)
		: m_Documents(Documents), m_Editors(Editors), m_Drafts(Drafts), m_Panel(Panel), m_AssetSelector(AssetSelector),
		  m_Style(Style), m_Actions(Actions), m_ProjectProvider(std::move(Project)), m_AssetListProvider(std::move(Assets))
	{
	}

	std::expected<void, std::string> EditorMaterialDocumentEditor::Register()
	{
		EditorDocumentEditorAdapter Adapter;
		Adapter.Kind = EditorDocumentKind::Material;
		Adapter.IsEligible = [this](const auto& Context) { return IsEligible(Context); };
		Adapter.LoadSession = [this](const auto& Context) { return LoadSession(Context); };
		Adapter.DrawWorkspace = [this](auto& Session) { DrawWorkspace(Session); };
		Adapter.Save = [this](auto& Session, uint64_t) { return SaveSession(Session); };
		Adapter.Discard = [this](auto& Session) { return DiscardSession(Session); };
		Adapter.FocusedCommand = [this](auto& Session, auto Command) { return DispatchFocusedCommand(Session, Command); };
		Adapter.AssetLocationChanged = [this](auto& Session, const auto& Path, std::string_view Name)
		{ return UpdateAssetLocation(Session, Path, Name); };
		Adapter.PrepareClose = [this](const auto& Context) { return PrepareClose(Context); };
		return m_Editors.Register(std::move(Adapter));
	}

	bool EditorMaterialDocumentEditor::Open(EditorDocumentSession& Session, const EditorDocumentEditorContext& Context) const
	{
		return Session.Identity.Kind == EditorDocumentKind::Material && m_Editors.Load(Session, Context);
	}

	bool EditorMaterialDocumentEditor::IsEligible(const EditorDocumentEditorContext& Context) const
	{
		if (Context.Identity.Kind != EditorDocumentKind::Material || !Context.Registry) return false;
		const auto Asset = Context.Registry->Find(Context.Identity.Asset);
		return Asset && PulseForge::HasAssetTypeExtension(Asset->ProjectRelativePath, PulseForge::AssetFileType::Material);
	}

	std::expected<std::unique_ptr<EditorDocumentEditorState>, std::string>
	EditorMaterialDocumentEditor::LoadSession(const EditorDocumentEditorContext& Context) const
	{
		PulseForge::Project* const Project = m_ProjectProvider ? m_ProjectProvider() : nullptr;
		if (!Project || Project->GetDescription().ID != Context.Identity.Project ||
			&Project->GetAssetRegistry() != Context.Registry)
			return std::unexpected("The active project changed while opening this material document");
		auto Draft = m_Drafts.LoadDraft(*Project, Context.Identity.Asset);
		if (!Draft) return std::unexpected("Could not open material draft: " + Draft.error());
		return std::unique_ptr<EditorDocumentEditorState>(std::make_unique<State>());
	}

	void EditorMaterialDocumentEditor::DrawWorkspace(EditorDocumentSession& Session)
	{
		State* const Current = dynamic_cast<State*>(Session.EditorState.get());
		PulseForge::Project* const Project = m_ProjectProvider ? m_ProjectProvider() : nullptr;
		if (!Current || !Project || Project->GetDescription().ID != Session.Identity.Project)
		{
			m_Style.EmptyState(EditorIcon::Warning, "The material document project is no longer active.");
			return;
		}
		if (ImGui::Button("Undo")) (void)DispatchFocusedCommand(Session, EditorDocumentCommand::Undo);
		ImGui::SameLine();
		if (ImGui::Button("Redo")) (void)DispatchFocusedCommand(Session, EditorDocumentCommand::Redo);
		ImGui::SameLine();
		ImGui::BeginDisabled(!Session.IsDirty());
		if (ImGui::Button("Save"))
		{
			if (!SaveSession(Session) && Current->Error.empty()) Current->Error = "The material could not be saved.";
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::BeginDisabled(!Session.IsDirty());
		if (ImGui::Button("Discard") && !DiscardSession(Session) && Current->Error.empty())
			Current->Error = "The material draft could not be reloaded.";
		ImGui::EndDisabled();
		ImGui::Separator();
		const std::vector<PulseForge::AssetRecord> Assets = m_AssetListProvider();
		m_Panel.DrawEditorContents(*Project, Session.Identity.Asset, Assets, m_Drafts,
			m_AssetSelector, m_Style, m_Actions);
		ImGui::SeparatorText("Draft Preview");
		if (m_Preview)
		{
			const ImVec2 Available = ImGui::GetContentRegionAvail();
			const float Width = (std::min)(Available.x, 480.0f);
			const float Height = Width * (240.0f / 320.0f);
			const bool Shown = m_Preview->Draw(Session.SessionGeneration, Session.Revision, Width, Height);
			const std::string& PreviewError = m_Preview->GetError(Session.SessionGeneration);
			if (!PreviewError.empty())
			{
				ImGui::TextWrapped(Shown ? "Last complete preview retained. New draft preview unavailable: %s" :
					"Preview unavailable: %s", PreviewError.c_str());
				if (ImGui::SmallButton("Retry Preview"))
					(void)m_Preview->RequestRetry(Session.SessionGeneration);
			}
			else if (!Shown) ImGui::TextDisabled("Preparing the fixed-PBR preview…");
		}
		else ImGui::TextDisabled("The material preview renderer is not available.");
		if (!Current->Error.empty()) ImGui::TextWrapped("%s", Current->Error.c_str());
	}

	bool EditorMaterialDocumentEditor::SaveSession(EditorDocumentSession& Session)
	{
		State* const Current = dynamic_cast<State*>(Session.EditorState.get());
		if (!Current || !m_Actions.SaveMaterialFromDetails(Session.Identity.Asset))
		{
			if (Current) Current->Error = "Save failed. The draft and history remain available for retry.";
			return false;
		}
		Current->Error.clear();
		return true;
	}

	bool EditorMaterialDocumentEditor::DiscardSession(EditorDocumentSession& Session)
	{
		State* const Current = dynamic_cast<State*>(Session.EditorState.get());
		if (!Current || !m_Actions.RevertMaterialFromDetails(Session.Identity.Asset))
		{
			if (Current) Current->Error = "Reload failed. The current draft remains available.";
			return false;
		}
		Current->Error.clear();
		return true;
	}

	bool EditorMaterialDocumentEditor::DispatchFocusedCommand(EditorDocumentSession& Session, EditorDocumentCommand Command)
	{
		PulseForge::Project* const Project = m_ProjectProvider ? m_ProjectProvider() : nullptr;
		if (!Project || Project->GetDescription().ID != Session.Identity.Project) return false;
		return m_Drafts.ApplyHistoryStep(*Project, Session.Identity.Asset, Command == EditorDocumentCommand::Redo, m_Documents)
			.value_or(false);
	}

	bool EditorMaterialDocumentEditor::UpdateAssetLocation(EditorDocumentSession& Session,
		const std::filesystem::path& Path, std::string_view)
	{
		return Session.Identity.Kind == EditorDocumentKind::Material && !Path.empty();
	}

	std::expected<void, std::string> EditorMaterialDocumentEditor::PrepareClose(const EditorDocumentCloseContext& Context) const
	{
		if (Context.Identity.Kind != EditorDocumentKind::Material || !Context.EditorState)
			return std::unexpected("The material document session is unavailable");
		return {};
	}
}
