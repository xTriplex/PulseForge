#pragma once

#include "Editor/EditorDocumentEditorRegistry.h"
#include "Editor/EditorMaterialPreviewService.h"

#include <functional>
#include <vector>

namespace PulseForge
{
	class Project;
}

namespace PulseForgeEditor
{
	class EditorAssetSelector;
	class EditorMaterialDetailsActionSink;
	class EditorMaterialDetailsPanel;
	class EditorMaterialDraftController;
	class EditorStyle;

	class EditorMaterialDocumentEditor final
	{
	public:
		using ProjectProvider = std::function<PulseForge::Project*()>;
		using AssetListProvider = std::function<std::vector<PulseForge::AssetRecord>()>;

		EditorMaterialDocumentEditor(EditorDocumentManager& Documents, EditorDocumentEditorRegistry& Editors,
			EditorMaterialDraftController& Drafts, EditorMaterialDetailsPanel& Panel, EditorAssetSelector& AssetSelector,
			EditorStyle& Style, EditorMaterialDetailsActionSink& Actions, ProjectProvider Project,
			AssetListProvider Assets);

		[[nodiscard]] std::expected<void, std::string> Register();
		[[nodiscard]] bool Open(EditorDocumentSession& Session, const EditorDocumentEditorContext& Context) const;
		void SetPreviewService(EditorMaterialPreviewService* Preview) noexcept { m_Preview = Preview; }

	private:
		struct State;
		[[nodiscard]] bool IsEligible(const EditorDocumentEditorContext& Context) const;
		[[nodiscard]] std::expected<std::unique_ptr<EditorDocumentEditorState>, std::string> LoadSession(
			const EditorDocumentEditorContext& Context) const;
		void DrawWorkspace(EditorDocumentSession& Session);
		[[nodiscard]] bool SaveSession(EditorDocumentSession& Session);
		[[nodiscard]] bool DiscardSession(EditorDocumentSession& Session);
		[[nodiscard]] bool DispatchFocusedCommand(EditorDocumentSession& Session, EditorDocumentCommand Command);
		[[nodiscard]] bool UpdateAssetLocation(EditorDocumentSession& Session,
			const std::filesystem::path& Path, std::string_view Name);
		[[nodiscard]] std::expected<void, std::string> PrepareClose(const EditorDocumentCloseContext& Context) const;

		EditorDocumentManager& m_Documents;
		EditorDocumentEditorRegistry& m_Editors;
		EditorMaterialDraftController& m_Drafts;
		EditorMaterialDetailsPanel& m_Panel;
		EditorAssetSelector& m_AssetSelector;
		EditorStyle& m_Style;
		EditorMaterialDetailsActionSink& m_Actions;
		ProjectProvider m_ProjectProvider;
		AssetListProvider m_AssetListProvider;
		EditorMaterialPreviewService* m_Preview = nullptr;
	};
}
