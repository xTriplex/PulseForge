#include "Editor/EditorSceneWorkspaceState.h"

namespace PulseForgeEditor
{
	void EditorSceneWorkspaceState::BeginHierarchyFrame()
	{
		m_CurrentHierarchyOrder.clear();
	}

	void EditorSceneWorkspaceState::RecordVisibleHierarchyEntity(const PulseForge::UUID& Identifier)
	{
		m_CurrentHierarchyOrder.push_back(Identifier);
	}

	void EditorSceneWorkspaceState::CompleteHierarchyFrame()
	{
		m_VisibleHierarchyOrder.swap(m_CurrentHierarchyOrder);
		m_CurrentHierarchyOrder.clear();
	}

	std::span<const PulseForge::UUID> EditorSceneWorkspaceState::VisibleHierarchyOrder() const noexcept
	{
		return m_VisibleHierarchyOrder;
	}

	void EditorSceneWorkspaceState::Reset() noexcept
	{
		m_Selection.Clear();
		m_Visibility.ShowAll();
		m_VisibleHierarchyOrder.clear();
		m_CurrentHierarchyOrder.clear();
	}
} // namespace PulseForgeEditor
