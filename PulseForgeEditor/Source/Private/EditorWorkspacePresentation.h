#pragma once

#include "Editor/EditorDocumentEditorRegistry.h"
#include "Editor/EditorWorkspaceFrameState.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace PulseForgeEditor
{
	class EditorWorkspacePresentation final
	{
	public:
		using FallbackDrawer = void (*)(void*, const EditorDocumentIdentity&);

		[[nodiscard]] bool BeginMainFrame(const std::optional<EditorDocumentIdentity>& ActiveDocument);
		[[nodiscard]] bool IsLevelWorkspaceActive() const noexcept;
		[[nodiscard]] bool WasWorkspaceDrawn(const EditorDocumentIdentity& Identity, std::string_view HostID) const;

		void DrawWorkspaceDockspace(EditorDocumentSession& Session);
		void DrawWorkspaceContent(EditorDocumentSession& Session, const EditorDocumentEditorRegistry& Editors, FallbackDrawer DrawFallback,
			void* FallbackContext);
		void KeepInactiveWorkspacesAlive(const EditorDocumentManager& Documents, const EditorDocumentEditorRegistry& Editors);

	private:
		[[nodiscard]] static uint32_t GetDockspaceID(const EditorDocumentIdentity& Identity);
		[[nodiscard]] static std::string GetWindowName(const EditorDocumentIdentity& Identity);

		EditorWorkspaceFrameState m_FrameState;
	};
} // namespace PulseForgeEditor
