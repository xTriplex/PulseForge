#pragma once

#include "Editor/EditorDocumentEditorRegistry.h"
#include "Editor/EditorStyle.h"

#include <array>
#include <cstddef>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace PulseForgeEditor::Detail
{
	class EditorDocumentHostTestEditor final
	{
	public:
		struct ModelSnapshot
		{
			int Value = 0;
			size_t UndoCount = 0;
			size_t RedoCount = 0;
		};

		EditorDocumentHostTestEditor(EditorDocumentManager& Documents, EditorDocumentEditorRegistry& Editors, EditorStyle& Style,
			std::function<void(std::string)> ReportError, std::function<PulseForge::UUID()> ActiveProject,
			std::function<void(std::string_view, int, int, bool)> ReportCloseUiAllowClose = {},
			std::function<void(std::string_view, int, int, bool, bool, bool, bool, int)> ReportCloseUiTextField = {},
			bool TextOnlyEditing = false);

		[[nodiscard]] std::expected<void, std::string> Register();
		[[nodiscard]] bool IsFixtureIdentity(const EditorDocumentIdentity& Identity) const;
		[[nodiscard]] static constexpr std::array<std::string_view, 2> FixtureAssetIDs() noexcept
		{
			return { "d3110000-0000-4000-8000-000000000001", "d3110000-0000-4000-8000-000000000002" };
		}
		[[nodiscard]] static std::optional<size_t> FindFixtureIndex(const PulseForge::UUID& Asset);
		[[nodiscard]] bool InitializeSavedModel(EditorDocumentSession& Session, int Value);
		[[nodiscard]] bool EditValue(EditorDocumentSession& Session, int Value);
		[[nodiscard]] bool FailNextSave(EditorDocumentSession& Session) noexcept;
		[[nodiscard]] bool SetCloseFailure(EditorDocumentSession& Session, bool ShouldFail) noexcept;
		[[nodiscard]] std::optional<ModelSnapshot> InspectModel(const EditorDocumentSession& Session) const noexcept;
		[[nodiscard]] std::optional<int> GetCurrentValue(const EditorDocumentIdentity& Identity) const noexcept;
		[[nodiscard]] std::optional<int> GetSavedValue(const EditorDocumentIdentity& Identity) const noexcept;
		[[nodiscard]] bool MatchesSavedModel(
			const EditorDocumentSession& Session, const EditorDocumentEditorState* ExpectedState, int ExpectedValue) const noexcept;

	private:
		struct State;

		EditorDocumentManager& m_Documents;
		EditorDocumentEditorRegistry& m_Editors;
		EditorStyle& m_Style;
		std::function<void(std::string)> m_ReportError;
		std::function<PulseForge::UUID()> m_ActiveProject;
		std::function<void(std::string_view, int, int, bool)> m_ReportCloseUiAllowClose;
		std::function<void(std::string_view, int, int, bool, bool, bool, bool, int)> m_ReportCloseUiTextField;
		bool m_TextOnlyEditing = false;
		std::unordered_map<EditorDocumentIdentity, int, EditorDocumentIdentityHash> m_SavedValues;
		std::unordered_map<EditorDocumentIdentity, int, EditorDocumentIdentityHash> m_LastKnownValues;
	};
} // namespace PulseForgeEditor::Detail
