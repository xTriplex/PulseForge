#pragma once

#include "Editor/EditorDocumentEditorRegistry.h"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>

namespace PulseForgeEditor
{
	enum class EditorGraphFixtureKind : uint8_t
	{
		MaterialExpression,
		GameplayEvent
	};

	struct EditorGraphFixtureLocation
	{
		PulseForge::UUID GraphID;
		std::filesystem::path ProjectRelativePath;
		std::string DisplayName;
	};

	class EditorGraphDocumentEditor final
	{
	public:
		EditorGraphDocumentEditor(EditorDocumentManager& Documents, EditorDocumentEditorRegistry& Editors);

		[[nodiscard]] std::expected<void, std::string> Register();
		[[nodiscard]] std::expected<EditorGraphFixtureLocation, std::string> CreateFixture(
			const std::filesystem::path& ProjectRoot, EditorGraphFixtureKind Kind) const;
		[[nodiscard]] static std::filesystem::path GetFixturePath(const PulseForge::UUID& GraphID);
		[[nodiscard]] bool Open(EditorDocumentSession& Session, const EditorDocumentEditorContext& Context) const;
		[[nodiscard]] bool Save(EditorDocumentSession& Session) const;

	private:
		[[nodiscard]] bool IsEligible(const EditorDocumentEditorContext& Context) const;
		[[nodiscard]] std::expected<std::unique_ptr<EditorDocumentEditorState>, std::string> LoadSession(
			const EditorDocumentEditorContext& Context) const;
		void DrawWorkspace(EditorDocumentSession& Session);
		[[nodiscard]] bool SaveSession(EditorDocumentSession& Session, uint64_t SavedRevision) const;
		[[nodiscard]] bool DiscardSession(EditorDocumentSession& Session) const;
		[[nodiscard]] bool HandleFocusedCommand(EditorDocumentSession& Session, EditorDocumentCommand Command) const;
		[[nodiscard]] bool UpdateAssetLocation(EditorDocumentSession& Session,
			const std::filesystem::path& Path, std::string_view DisplayName) const;
		[[nodiscard]] std::expected<void, std::string> PrepareClose(const EditorDocumentCloseContext& Context) const;
		[[nodiscard]] bool ReloadState(EditorDocumentSession& Session) const;

		EditorDocumentManager& m_Documents;
		EditorDocumentEditorRegistry& m_Editors;
		bool m_Registered = false;
	};
}
