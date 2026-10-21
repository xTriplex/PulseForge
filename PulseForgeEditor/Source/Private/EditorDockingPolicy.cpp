#include "Editor/EditorDockingPolicy.h"
#include "Editor/EditorLayout.h"

#include <imgui_internal.h>

#include <string_view>

namespace PulseForgeEditor
{
	namespace
	{
		bool WindowBelongsToDomain(const ImGuiWindow* Window, EditorDockingDomain Domain)
		{
			if (!Window) return false;
			const std::string_view Name(Window->Name, static_cast<size_t>(ImStrlen(Window->Name)));
			if (Name == "##PFMainDocumentWorkspace" || Name.starts_with("Document Workspace###PFDocumentWorkspace_"))
				return Domain == EditorDockingDomain::AssetDocument;

			for (const EditorPanelDescriptor& Descriptor : EditorPanelDescriptors)
			{
				if (Name == Descriptor.WindowName || Name.ends_with("###" + std::string(Descriptor.WindowName)))
					return Descriptor.IsUtilityPanel == (Domain == EditorDockingDomain::UtilityPanel);
			}

			const ImGuiWindowClass& WindowClass = Window->WindowClass;
			if (WindowClass.ClassId == 0) return false;
			return IsEditorWindowClassInDomain(WindowClass, Domain);
		}

		bool NodeTreeBelongsToDomain(const ImGuiDockNode* Node, EditorDockingDomain Domain)
		{
			if (!Node) return true;
			for (int Index = 0; Index < Node->Windows.Size; ++Index)
				if (!WindowBelongsToDomain(Node->Windows[Index], Domain)) return false;
			return NodeTreeBelongsToDomain(Node->ChildNodes[0], Domain) &&
				   NodeTreeBelongsToDomain(Node->ChildNodes[1], Domain);
		}
	}

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
		if (!Root) return true;
		if (Root->WindowClass.ClassId != 0 && !IsEditorWindowClassInDomain(Root->WindowClass, Domain))
		{
			ImGui::DockContextQueueUndockWindow(ImGui::GetCurrentContext(), Window);
			return false;
		}

		// Dock settings restore node membership before the first Begin() reapplies a
		// window's class. A floating root can therefore still have ClassId == 0 even
		// though each attached editor window has a known domain from its identity.
		// Validate every member so a mixed root created by settings or DockBuilder
		// cannot inherit permission from whichever member updated the root first.
		if (NodeTreeBelongsToDomain(Root, Domain)) return true;

		ImGui::DockContextQueueUndockWindow(ImGui::GetCurrentContext(), Window);
		return false;
	}
}
