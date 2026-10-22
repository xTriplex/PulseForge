#include "Editor/EditorWorkspacePresentation.h"
#include "Editor/EditorDockingPolicy.h"

#include <imgui_internal.h>

namespace PulseForgeEditor
{
	bool EditorWorkspacePresentation::BeginMainFrame(const std::optional<EditorDocumentIdentity>& ActiveDocument)
	{
		return m_FrameState.BeginFrame(ActiveDocument);
	}

	bool EditorWorkspacePresentation::IsLevelWorkspaceActive() const noexcept
	{
		return m_FrameState.IsLevelWorkspaceActive();
	}

	bool EditorWorkspacePresentation::WasWorkspaceDrawn(const EditorDocumentIdentity& Identity, std::string_view HostID) const
	{
		return m_FrameState.WasWorkspaceDrawn(Identity, HostID);
	}

	void EditorWorkspacePresentation::DrawWorkspaceDockspace(EditorDocumentSession& Session)
	{
		const ImGuiID DockspaceID = GetDockspaceID(Session.Identity);
		const std::string WindowName = GetWindowName(Session.Identity);
		const ImVec2 Position = ImGui::GetCursorScreenPos();
		const ImVec2 Size = ImGui::GetContentRegionAvail();
		if (!ImGui::DockBuilderGetNode(DockspaceID))
		{
			ImGui::DockBuilderAddNode(DockspaceID, ImGuiDockNodeFlags_DockSpace);
			ImGui::DockBuilderSetNodePos(DockspaceID, Position);
			ImGui::DockBuilderSetNodeSize(DockspaceID, ImVec2((std::max)(Size.x, 1.0f), (std::max)(Size.y, 1.0f)));
			ImGui::DockBuilderDockWindow(WindowName.c_str(), DockspaceID);
			ImGui::DockBuilderFinish(DockspaceID);
		}
		const ImGuiWindowClass DocumentClass = MakeEditorDockingClass(EditorDockingDomain::AssetDocument);
		ImGui::DockSpace(DockspaceID, Size, ImGuiDockNodeFlags_None, &DocumentClass);
		m_FrameState.MarkDockspaceDrawn(Session.Identity);
	}

	void EditorWorkspacePresentation::DrawWorkspaceContent(
		EditorDocumentSession& Session, const EditorDocumentEditorRegistry& Editors, FallbackDrawer DrawFallback, void* FallbackContext)
	{
		const std::string WindowName = GetWindowName(Session.Identity);
		const ImGuiWindowClass DocumentClass = MakeEditorDockingClass(EditorDockingDomain::AssetDocument);
		ImGui::SetNextWindowClass(&DocumentClass);
		const bool ContentsVisible = ImGui::Begin(WindowName.c_str(), nullptr, ImGuiWindowFlags_NoCollapse);
		const bool DockingDomainValid = EnforceCurrentEditorWindowDockingDomain(EditorDockingDomain::AssetDocument);
		if (ContentsVisible && DockingDomainValid)
		{
			(void)EnforceCurrentEditorWindowDockingDomain(EditorDockingDomain::AssetDocument);
			if (Editors.DrawWorkspace(Session)) m_FrameState.MarkWorkspaceDrawn(Session.Identity, Session.HostID);
			else if (DrawFallback) DrawFallback(FallbackContext, Session.Identity);
		}
		ImGui::End();
	}

	void EditorWorkspacePresentation::KeepInactiveWorkspacesAlive(
		const EditorDocumentManager& Documents, const EditorDocumentEditorRegistry& Editors)
	{
		if (!ImGui::GetCurrentContext()) return;

		bool HasInactiveWorkspace = false;
		for (const auto& [Identity, Session] : Documents.GetSessions())
		{
			(void)Session;
			if (!Editors.IsRegistered(Identity.Kind) || m_FrameState.WasDockspaceDrawn(Identity)) continue;
			HasInactiveWorkspace = true;
			break;
		}
		if (!HasInactiveWorkspace) return;

		const ImGuiViewport* MainViewport = ImGui::GetMainViewport();
		if (!MainViewport) return;
		ImGui::SetNextWindowViewport(MainViewport->ID);
		ImGui::SetNextWindowPos(ImVec2(-10000.0f, -10000.0f), ImGuiCond_Always);
		ImGui::SetNextWindowSize(ImVec2(1.0f, 1.0f), ImGuiCond_Always);
		if (!ImGui::Begin("##PFInactiveDocumentWorkspaceKeepAlive", nullptr,
				ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings |
					ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBackground))
		{
			ImGui::End();
			return;
		}
		for (const auto& [Identity, Session] : Documents.GetSessions())
		{
			(void)Session;
			if (!Editors.IsRegistered(Identity.Kind) || m_FrameState.WasDockspaceDrawn(Identity)) continue;
			const ImGuiWindowClass DocumentClass = MakeEditorDockingClass(EditorDockingDomain::AssetDocument);
			ImGui::DockSpace(GetDockspaceID(Identity), ImVec2(1.0f, 1.0f), ImGuiDockNodeFlags_KeepAliveOnly, &DocumentClass);
		}
		ImGui::End();
	}

	uint32_t EditorWorkspacePresentation::GetDockspaceID(const EditorDocumentIdentity& Identity)
	{
		const std::string Key = EditorWorkspaceFrameState::GetWorkspaceKey(Identity);
		return ImHashStr(Key.c_str());
	}

	std::string EditorWorkspacePresentation::GetWindowName(const EditorDocumentIdentity& Identity)
	{
		return "Document Workspace###PFDocumentWorkspace_" + Identity.Project.ToString() + "_" + Identity.Asset.ToString() + "_" +
			   std::to_string(static_cast<uint32_t>(Identity.Kind));
	}
} // namespace PulseForgeEditor
