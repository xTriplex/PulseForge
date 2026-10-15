#pragma once

#include "Editor/EditorDocumentManager.h"

#include <optional>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace PulseForgeEditor
{
	class EditorWorkspaceFrameState final
	{
	public:
		[[nodiscard]] bool BeginFrame(const std::optional<EditorDocumentIdentity>& ActiveDocument)
		{
			const bool ActiveDocumentChanged = ActiveDocument != m_LastActiveDocument;
			m_LastActiveDocument = ActiveDocument;
			m_LevelWorkspaceActive = !ActiveDocument || ActiveDocument->Kind == EditorDocumentKind::Level;
			m_DrawnDockspaces.clear();
			m_DrawnWorkspaceHosts.clear();
			return ActiveDocumentChanged;
		}

		[[nodiscard]] bool IsLevelWorkspaceActive() const noexcept { return m_LevelWorkspaceActive; }

		void MarkDockspaceDrawn(const EditorDocumentIdentity& Identity) { m_DrawnDockspaces.insert(Identity); }

		[[nodiscard]] bool WasDockspaceDrawn(const EditorDocumentIdentity& Identity) const { return m_DrawnDockspaces.contains(Identity); }

		void MarkWorkspaceDrawn(const EditorDocumentIdentity& Identity, std::string HostID)
		{
			m_DrawnWorkspaceHosts.insert_or_assign(Identity, std::move(HostID));
		}

		[[nodiscard]] bool WasWorkspaceDrawn(const EditorDocumentIdentity& Identity, std::string_view HostID) const
		{
			const auto Found = m_DrawnWorkspaceHosts.find(Identity);
			return Found != m_DrawnWorkspaceHosts.end() && Found->second == HostID;
		}

		[[nodiscard]] static std::string GetWorkspaceKey(const EditorDocumentIdentity& Identity)
		{
			return "PulseForge.DocumentWorkspace/" + Identity.Project.ToString() + "/" + Identity.Asset.ToString() + "/" +
				   std::to_string(static_cast<uint32_t>(Identity.Kind));
		}

	private:
		std::optional<EditorDocumentIdentity> m_LastActiveDocument;
		std::unordered_set<EditorDocumentIdentity, EditorDocumentIdentityHash> m_DrawnDockspaces;
		std::unordered_map<EditorDocumentIdentity, std::string, EditorDocumentIdentityHash> m_DrawnWorkspaceHosts;
		bool m_LevelWorkspaceActive = true;
	};
} // namespace PulseForgeEditor
