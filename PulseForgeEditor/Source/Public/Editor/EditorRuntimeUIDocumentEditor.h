#pragma once

#include "Editor/EditorDocumentEditorRegistry.h"
#include "UI/RuntimeUI.h"

#include <functional>
#include <optional>
#include <string_view>

namespace PulseForgeEditor
{
	class EditorStyle;

	enum class RuntimeUIDocumentSaveStage : uint8_t
	{
		BeforeTemporaryCreate,
		BeforeTemporaryWrite,
		BeforeAtomicReplace
	};

	using RuntimeUIDocumentSaveFailureHook =
		std::function<std::optional<std::string>(RuntimeUIDocumentSaveStage)>;

	class EditorRuntimeUIDocumentEditor final
	{
	public:
		EditorRuntimeUIDocumentEditor(
			EditorDocumentManager& Documents,
			EditorDocumentEditorRegistry& Editors,
			EditorStyle& Style,
			RuntimeUIDocumentSaveFailureHook SaveFailureHook = {});
		[[nodiscard]] std::expected<void, std::string> Register();
		[[nodiscard]] bool Open(EditorDocumentSession& Session, const EditorDocumentEditorContext& Context) const;
		[[nodiscard]] bool DrawWorkspace(EditorDocumentSession& Session);
		[[nodiscard]] bool Save(EditorDocumentSession& Session);
		[[nodiscard]] bool Discard(EditorDocumentSession& Session);
		[[nodiscard]] const PulseForge::RuntimeUIDocument* GetDraft(const EditorDocumentSession& Session) const noexcept;
		[[nodiscard]] std::string_view GetLastError(const EditorDocumentSession& Session) const noexcept;
		[[nodiscard]] std::expected<void, std::string> ApplyDraftEdit(
			EditorDocumentSession& Session, PulseForge::RuntimeUIDocument Draft);
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
		RuntimeUIDocumentSaveFailureHook m_SaveFailureHook;
	};
}
