#include "Editor/EditorDockingPolicy.h"

#include <imgui_internal.h>

namespace PulseForgeEditor
{
	ImGuiWindowClass MakeEditorDockingClass(EditorDockingDomain Domain) noexcept
	{
		ImGuiWindowClass WindowClass;
		WindowClass.ClassId = Domain == EditorDockingDomain::UtilityPanel ? 0x50465554u : 0x5046444Fu;
		WindowClass.DockingAllowUnclassed = false;
		return WindowClass;
	}

	bool IsEditorWindowClassInDomain(const ImGuiWindowClass& WindowClass, EditorDockingDomain Domain) noexcept
	{
		return WindowClass.ClassId == MakeEditorDockingClass(Domain).ClassId;
	}

	bool IsEditorDockingDomainCompatible(EditorDockingDomain Source, EditorDockingDomain Destination) noexcept
	{
		return Source == Destination;
	}

	bool EnforceCurrentEditorWindowDockingDomain(EditorDockingDomain Domain)
	{
		ImGuiWindow* Window = ImGui::GetCurrentWindow();
		ImGuiDockNode* Node = Window ? Window->DockNode : nullptr;
		if (!Node) return true;

		const ImGuiDockNode* Root = ImGui::DockNodeGetRootNode(Node);
		if (!Root || IsEditorWindowClassInDomain(Root->WindowClass, Domain)) return true;

		ImGui::DockContextQueueUndockWindow(ImGui::GetCurrentContext(), Window);
		return false;
	}
}
