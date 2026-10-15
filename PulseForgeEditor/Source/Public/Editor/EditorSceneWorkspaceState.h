#pragma once

#include "Editor/EditorSelection.h"
#include "Editor/EditorVisibility.h"

#include <span>
#include <vector>

namespace PulseForgeEditor
{
	// Owns scene-editing state shared by the Hierarchy, Inspector, and viewport.
	// Hierarchy order is committed once per frame so range selection never observes
	// a partially rebuilt traversal.
	class EditorSceneWorkspaceState final
	{
	public:
		[[nodiscard]] EditorSelection& Selection() noexcept { return m_Selection; }
		[[nodiscard]] const EditorSelection& Selection() const noexcept { return m_Selection; }
		[[nodiscard]] EditorVisibility& Visibility() noexcept { return m_Visibility; }
		[[nodiscard]] const EditorVisibility& Visibility() const noexcept { return m_Visibility; }

		void BeginHierarchyFrame();
		void RecordVisibleHierarchyEntity(const PulseForge::UUID& Identifier);
		void CompleteHierarchyFrame();
		[[nodiscard]] std::span<const PulseForge::UUID> VisibleHierarchyOrder() const noexcept;
		void Reset() noexcept;

	private:
		EditorSelection m_Selection;
		EditorVisibility m_Visibility;
		std::vector<PulseForge::UUID> m_VisibleHierarchyOrder;
		std::vector<PulseForge::UUID> m_CurrentHierarchyOrder;
	};
} // namespace PulseForgeEditor
