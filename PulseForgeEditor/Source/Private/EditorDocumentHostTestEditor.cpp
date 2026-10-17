#include "Editor/EditorDocumentHostTestEditor.h"

#include <imgui.h>

#include <utility>
#include <vector>

namespace PulseForgeEditor::Detail
{
	struct EditorDocumentHostTestEditor::State final : EditorDocumentEditorState
	{
		int Value = 0;
		bool FailNextSave = false;
		bool FailClose = false;
		std::vector<int> Undo;
		std::vector<int> Redo;
	};

	EditorDocumentHostTestEditor::EditorDocumentHostTestEditor(EditorDocumentManager& Documents, EditorDocumentEditorRegistry& Editors,
		EditorStyle& Style, std::function<void(std::string)> ReportError, std::function<PulseForge::UUID()> ActiveProject)
		: m_Documents(Documents), m_Editors(Editors), m_Style(Style), m_ReportError(std::move(ReportError)),
		  m_ActiveProject(std::move(ActiveProject))
	{
	}

	std::expected<void, std::string> EditorDocumentHostTestEditor::Register()
	{
		EditorDocumentEditorAdapter Adapter;
		Adapter.Kind = EditorDocumentKind::Texture;
		Adapter.IsEligible = [this](const auto& Context) { return IsFixtureIdentity(Context.Identity); };
		Adapter.LoadSession = [this](const auto& Context) -> std::expected<std::unique_ptr<EditorDocumentEditorState>, std::string>
		{
			if (!IsFixtureIdentity(Context.Identity)) return std::unexpected("The identity is not a document-host test fixture");
			auto SessionState = std::make_unique<State>();
			if (const auto Saved = m_SavedValues.find(Context.Identity); Saved != m_SavedValues.end()) SessionState->Value = Saved->second;
			return std::unique_ptr<EditorDocumentEditorState>(std::move(SessionState));
		};
		Adapter.DrawWorkspace = [this](auto& Session)
		{
			auto* SessionState = dynamic_cast<State*>(Session.EditorState.get());
			if (!SessionState) return;
			(void)m_Style.SectionHeader(EditorIcon::File, "Registered Document Workspace Fixture");
			m_Style.TextMuted("This independent workspace is not part of the Level Editor panels.");
			ImGui::Text("Asset %s", Session.Identity.Asset.ToString().c_str());
			int Candidate = SessionState->Value;
			if (ImGui::InputInt("Editable value", &Candidate)) (void)EditValue(Session, Candidate);
			if (ImGui::Button("Save fixture")) (void)m_Editors.Save(Session);
			ImGui::SameLine();
			if (ImGui::Button("Revert fixture")) (void)m_Editors.Discard(Session);
			ImGui::SameLine();
			if (ImGui::Button("Fail next save")) (void)FailNextSave(Session);
			ImGui::SameLine();
			if (ImGui::Button(SessionState->FailClose ? "Allow close" : "Fail close"))
				(void)SetCloseFailure(Session, !SessionState->FailClose);
			ImGui::Text("Undo %zu  |  Redo %zu", SessionState->Undo.size(), SessionState->Redo.size());
			ImGui::TextUnformatted(Session.IsDirty() ? "Unsaved fixture changes" : "Fixture saved");
		};
		Adapter.Save = [this](auto& Session, uint64_t)
		{
			auto* SessionState = dynamic_cast<State*>(Session.EditorState.get());
			if (!SessionState || SessionState->FailNextSave)
			{
				if (SessionState) SessionState->FailNextSave = false;
				if (m_ReportError) m_ReportError("The document-host fixture save was rejected.");
				return false;
			}
			m_SavedValues[Session.Identity] = SessionState->Value;
			return true;
		};
		Adapter.Discard = [this](auto& Session)
		{
			auto* SessionState = dynamic_cast<State*>(Session.EditorState.get());
			if (!SessionState) return false;
			if (const auto Saved = m_SavedValues.find(Session.Identity); Saved != m_SavedValues.end()) SessionState->Value = Saved->second;
			else SessionState->Value = 0;
			SessionState->Undo.clear();
			SessionState->Redo.clear();
			return true;
		};
		Adapter.FocusedCommand = [this](auto& Session, EditorDocumentCommand Command)
		{
			auto* SessionState = dynamic_cast<State*>(Session.EditorState.get());
			if (!SessionState) return false;
			auto& Source = Command == EditorDocumentCommand::Undo ? SessionState->Undo : SessionState->Redo;
			auto& Destination = Command == EditorDocumentCommand::Undo ? SessionState->Redo : SessionState->Undo;
			if (Source.empty() || !m_Documents.MarkEdited(Session.Identity)) return false;
			Destination.push_back(SessionState->Value);
			SessionState->Value = Source.back();
			Source.pop_back();
			return true;
		};
		Adapter.AssetLocationChanged = [](auto&, const auto&, std::string_view) { return true; };
		Adapter.PrepareClose = [](const auto& Session) -> std::expected<void, std::string>
		{
			const auto* SessionState = dynamic_cast<const State*>(Session.EditorState.get());
			if (SessionState && SessionState->FailClose) return std::unexpected("The Debug fixture is configured to reject close.");
			return {};
		};
		Adapter.Close = [](auto&) {};
		return m_Editors.Register(std::move(Adapter));
	}

	bool EditorDocumentHostTestEditor::IsFixtureIdentity(const EditorDocumentIdentity& Identity) const
	{
		if (!m_ActiveProject || Identity.Project != m_ActiveProject() || Identity.Kind != EditorDocumentKind::Texture) return false;
		return FindFixtureIndex(Identity.Asset).has_value();
	}

	std::optional<size_t> EditorDocumentHostTestEditor::FindFixtureIndex(const PulseForge::UUID& Asset)
	{
		const auto FixtureIDs = FixtureAssetIDs();
		for (size_t Index = 0; Index < FixtureIDs.size(); ++Index)
			if (const auto Parsed = PulseForge::UUID::Parse(FixtureIDs[Index]); Parsed && *Parsed == Asset) return Index;
		return std::nullopt;
	}

	bool EditorDocumentHostTestEditor::InitializeSavedModel(EditorDocumentSession& Session, int Value)
	{
		auto* SessionState = dynamic_cast<State*>(Session.EditorState.get());
		if (!SessionState) return false;
		SessionState->Value = Value;
		SessionState->Undo.push_back(0);
		return m_Documents.MarkEdited(Session.Identity) && m_Editors.Save(Session);
	}

	bool EditorDocumentHostTestEditor::EditValue(EditorDocumentSession& Session, int Value)
	{
		auto* SessionState = dynamic_cast<State*>(Session.EditorState.get());
		if (!SessionState || SessionState->Value == Value || !m_Documents.MarkEdited(Session.Identity)) return false;
		SessionState->Undo.push_back(SessionState->Value);
		SessionState->Redo.clear();
		SessionState->Value = Value;
		return true;
	}

	bool EditorDocumentHostTestEditor::FailNextSave(EditorDocumentSession& Session) noexcept
	{
		auto* SessionState = dynamic_cast<State*>(Session.EditorState.get());
		if (!SessionState) return false;
		SessionState->FailNextSave = true;
		return true;
	}

	bool EditorDocumentHostTestEditor::SetCloseFailure(EditorDocumentSession& Session, bool ShouldFail) noexcept
	{
		auto* SessionState = dynamic_cast<State*>(Session.EditorState.get());
		if (!SessionState) return false;
		SessionState->FailClose = ShouldFail;
		return true;
	}

	std::optional<EditorDocumentHostTestEditor::ModelSnapshot> EditorDocumentHostTestEditor::InspectModel(
		const EditorDocumentSession& Session) const noexcept
	{
		const auto* SessionState = dynamic_cast<const State*>(Session.EditorState.get());
		if (!SessionState) return std::nullopt;
		return ModelSnapshot{ SessionState->Value, SessionState->Undo.size(), SessionState->Redo.size() };
	}

	bool EditorDocumentHostTestEditor::MatchesSavedModel(
		const EditorDocumentSession& Session, const EditorDocumentEditorState* ExpectedState, int ExpectedValue) const noexcept
	{
		const auto* SessionState = dynamic_cast<const State*>(Session.EditorState.get());
		return ExpectedState && Session.EditorState.get() == ExpectedState && SessionState && SessionState->Value == ExpectedValue &&
			   SessionState->Undo.size() == 1 && SessionState->Undo.front() == 0 && SessionState->Redo.empty() && !Session.IsDirty();
	}
} // namespace PulseForgeEditor::Detail
