#pragma once

#include <imgui.h>

namespace PulseForgeEditor
{
	enum class EditorDockingDomain
	{
		UtilityPanel,
		AssetDocument
	};

	[[nodiscard]] ImGuiWindowClass MakeEditorDockingClass(EditorDockingDomain Domain) noexcept;
	[[nodiscard]] bool IsEditorWindowClassInDomain(const ImGuiWindowClass& WindowClass, EditorDockingDomain Domain) noexcept;
	[[nodiscard]] bool IsEditorDockingDomainCompatible(EditorDockingDomain Source, EditorDockingDomain Destination) noexcept;
	[[nodiscard]] bool EnforceCurrentEditorWindowDockingDomain(EditorDockingDomain Domain);
}
