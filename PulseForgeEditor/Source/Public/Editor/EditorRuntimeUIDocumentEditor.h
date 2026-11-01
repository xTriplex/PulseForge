#pragma once

#include "Editor/EditorDocumentEditorRegistry.h"

namespace PulseForgeEditor
{
	class EditorStyle;

	class EditorRuntimeUIDocumentEditor final
	{
	public:
		EditorRuntimeUIDocumentEditor(EditorDocumentManager& Documents, EditorDocumentEditorRegistry& Editors, EditorStyle& Style);
		[[nodiscard]] std::expected<void, std::string> Register();
		[[nodiscard]] bool Open(EditorDocumentSession& Session, const EditorDocumentEditorContext& Context) const;
		[[nodiscard]] bool DrawWorkspace(EditorDocumentSession& Session);
		[[nodiscard]] bool Save(EditorDocumentSession& Session);
		[[nodiscard]] bool Discard(EditorDocumentSession& Session);
		[[nodiscard]] bool DispatchFocusedCommand(EditorDocumentSession& Session, EditorDocumentCommand Command);
		[[nodiscard]] std::expected<void, std::string> PrepareClose(const EditorDocumentCloseContext& Context) const;
		struct State;

	private:
		[[nodiscard]] bool IsEligible(const EditorDocumentEditorContext& Context) const;
		[[nodiscard]] std::expected<std::unique_ptr<EditorDocumentEditorState>, std::string> LoadSession(
			const EditorDocumentEditorContext& Context) const;
		[[nodiscard]] bool SaveSession(EditorDocumentSession& Session, uint64_t SavedRevision);
		[[nodiscard]] bool DiscardSession(EditorDocumentSession& Session);
		[[nodiscard]] bool UpdateAssetLocation(EditorDocumentSession& Session,
			const std::filesystem::path& Path, std::string_view Name);

		EditorDocumentManager& m_Documents;
		EditorDocumentEditorRegistry& m_Editors;
		EditorStyle& m_Style;
	};
}
