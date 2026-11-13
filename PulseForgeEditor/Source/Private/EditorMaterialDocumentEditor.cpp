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
		ImGui::SameLine();
		if (Session.IsDirty()) ImGui::TextDisabled("Unsaved changes");
		else ImGui::TextDisabled("Saved");
		ImGui::Separator();
		const std::vector<PulseForge::AssetRecord> Assets = m_AssetListProvider();
		m_Panel.DrawEditorContents(*Project, Session.Identity.Asset, Assets, m_Drafts,
			m_AssetSelector, m_Style, m_Actions, false);
		if (PulseForge::MaterialAssetDesc* Draft = m_Drafts.FindDraft(*Project, Session.Identity.Asset))
		{
			ImGui::SeparatorText("Material Graph Source");
			if (!Draft->Graph)
			{
				if (ImGui::Button("Create scalar PBR graph"))
				{
					const auto GraphID = PulseForge::UUID::Generate();
					const auto ConstantID = PulseForge::UUID::Generate();
					const auto ZeroID = PulseForge::UUID::Generate();
					const auto AddID = PulseForge::UUID::Generate();
					const auto OutputID = PulseForge::UUID::Generate();
					const auto FirstConnectionID = PulseForge::UUID::Generate();
					const auto SecondConnectionID = PulseForge::UUID::Generate();
					const auto OutputConnectionID = PulseForge::UUID::Generate();
					if (!GraphID || !ConstantID || !ZeroID || !AddID || !OutputID || !FirstConnectionID ||
						!SecondConnectionID || !OutputConnectionID)
						Current->Error = "Could not allocate stable graph source identities.";
					else
					{
						const PulseForge::MaterialAssetDesc Before = *Draft;
						PulseForge::MaterialGraphSource Graph;
						Graph.GraphID = *GraphID;
						Graph.Nodes = {
							{ *ConstantID, PulseForge::MaterialGraphNodeKind::FloatConstant, Draft->BaseColorFactor.r, {} },
							{ *ZeroID, PulseForge::MaterialGraphNodeKind::FloatConstant, 0.0f, {} },
							{ *AddID, PulseForge::MaterialGraphNodeKind::Add, 0.0f, {} },
							{ *OutputID, PulseForge::MaterialGraphNodeKind::PbrOutput, 0.0f, {} }
						};
						Graph.Connections = {
							{ *FirstConnectionID, *ConstantID, *AddID, 0 },
							{ *SecondConnectionID, *ZeroID, *AddID, 1 },
							{ *OutputConnectionID, *AddID, *OutputID, 0 }
						};
						Draft->Graph = std::move(Graph);
						if (m_Drafts.BeginEdit(*Project, Session.Identity.Asset, Before))
							(void)m_Documents.MarkEdited(Session.Identity);
						Current->Error.clear();
					}
				}
				ImGui::TextWrapped("Creates a versioned constant-plus-add source graph from the material's current red base-color value. The graph source is saved with this Material asset.");
			}
			else
			{
				const auto Validation = PulseForge::ValidateMaterialGraphSource(*Draft->Graph);
				if (!Validation.IsValid())
					ImGui::TextColored(m_Style.GetColor(EditorColorToken::Error), "Graph source is invalid: %s",
						Validation.Diagnostics.front().Message.c_str());
				else
					ImGui::TextDisabled("Graph source is valid. Cooking compiles this graph to a Vulkan SPIR-V product.");
				auto Constant = std::find_if(Draft->Graph->Nodes.begin(), Draft->Graph->Nodes.end(), [](const auto& Node)
					{ return Node.Kind == PulseForge::MaterialGraphNodeKind::FloatConstant; });
				if (Constant != Draft->Graph->Nodes.end())
				{
					float Value = Constant->Value;
					ImGui::SetNextItemWidth(240.0f);
					if (ImGui::DragFloat("PBR base-color scalar", &Value, 0.01f, 0.0f, 1.0f, "%.3f"))
					{
						const PulseForge::MaterialAssetDesc Before = *Draft;
						Constant->Value = Value;
						if (m_Drafts.BeginEdit(*Project, Session.Identity.Asset, Before))
							(void)m_Documents.MarkEdited(Session.Identity);
					}
				}
				ImGui::TextDisabled("This first graph slice maps a scalar expression into grayscale base color; fixed material values remain available in Details.");
			}
		}
		ImGui::SeparatorText("Draft Preview");
		if (m_Preview)
		{
			const ImVec2 Available = ImGui::GetContentRegionAvail();
			const float Width = (std::min)(Available.x, 480.0f);
			const float Height = Width * (240.0f / 320.0f);
			const uint64_t DraftContentRevision = m_Drafts.GetContentRevision(*Project, Session.Identity.Asset);
			const bool Shown = m_Preview->Draw(Session.SessionGeneration, DraftContentRevision, Width, Height);
			const std::string& PreviewError = m_Preview->GetError(Session.SessionGeneration);
			if (!PreviewError.empty())
			{
				ImGui::TextWrapped(Shown ? "Last complete preview retained. New draft preview unavailable: %s" :
					"Preview unavailable: %s", PreviewError.c_str());
				if (ImGui::SmallButton("Retry Preview"))
					(void)m_Preview->RequestRetry(Session.SessionGeneration, DraftContentRevision);
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
