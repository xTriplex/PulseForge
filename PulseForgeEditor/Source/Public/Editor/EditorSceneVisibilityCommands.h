#pragma once

#include "Editor/EditorSceneCommandActions.h"
#include "Editor/EditorVisibility.h"
#include "Scene/Scene.h"

#include <vector>

namespace PulseForgeEditor
{
	class EditorSceneWorkspaceState;

	class EditorSceneVisibilityCommands final
	{
	public:
		[[nodiscard]] static bool IsEntityEffectivelyVisible(const PulseForge::Entity& Entity, const EditorVisibility& Visibility);
		[[nodiscard]] static std::vector<PulseForge::UUID> GetEffectivelyHiddenEntities(
			const PulseForge::Scene* SceneValue, const EditorVisibility& Visibility);

		void ToggleEntityVisibility(PulseForge::Scene* SceneValue, EditorSceneWorkspaceState& Workspace, const PulseForge::UUID& Identifier,
			bool RuntimeActive, EditorSceneCommandActions& Actions) const;
		void HideSelectedEntities(PulseForge::Scene* SceneValue, EditorSceneWorkspaceState& Workspace, bool RuntimeActive,
			EditorSceneCommandActions& Actions) const;
		void ShowAllEntities(PulseForge::Scene* SceneValue, EditorSceneWorkspaceState& Workspace, bool RuntimeActive,
			EditorSceneCommandActions& Actions) const;
		void IsolateSelectedEntities(PulseForge::Scene* SceneValue, EditorSceneWorkspaceState& Workspace, bool RuntimeActive,
			EditorSceneCommandActions& Actions) const;
		void RestoreIsolatedVisibility(PulseForge::Scene* SceneValue, EditorSceneWorkspaceState& Workspace, bool RuntimeActive,
			EditorSceneCommandActions& Actions) const;
	};
} // namespace PulseForgeEditor
