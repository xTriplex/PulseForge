#pragma once

#include "Editor/EditorVisibility.h"
#include "Scene/Scene.h"

#include <expected>
#include <functional>
#include <string>
#include <vector>

namespace PulseForgeEditor
{
	class EditorSceneWorkspaceState;

	class EditorSceneVisibilityActions
	{
	public:
		virtual ~EditorSceneVisibilityActions() = default;
		[[nodiscard]] virtual std::expected<void, std::string> RunVisibilitySceneEdit(
			std::string Description,
			std::function<std::expected<void, PulseForge::SceneError>()> Operation) = 0;
		virtual void ReportVisibilityError(std::string Message) = 0;
		virtual void SetVisibilityStatus(std::string Message) = 0;
	};

	class EditorSceneVisibilityCommands final
	{
	public:
		[[nodiscard]] static bool IsEntityEffectivelyVisible(
			const PulseForge::Entity& Entity,
			const EditorVisibility& Visibility);
		[[nodiscard]] static std::vector<PulseForge::UUID> GetEffectivelyHiddenEntities(
			const PulseForge::Scene* SceneValue,
			const EditorVisibility& Visibility);

		void ToggleEntityVisibility(
			PulseForge::Scene* SceneValue,
			EditorSceneWorkspaceState& Workspace,
			const PulseForge::UUID& Identifier,
			bool RuntimeActive,
			EditorSceneVisibilityActions& Actions) const;
		void HideSelectedEntities(
			PulseForge::Scene* SceneValue,
			EditorSceneWorkspaceState& Workspace,
			bool RuntimeActive,
			EditorSceneVisibilityActions& Actions) const;
		void ShowAllEntities(
			PulseForge::Scene* SceneValue,
			EditorSceneWorkspaceState& Workspace,
			bool RuntimeActive,
			EditorSceneVisibilityActions& Actions) const;
		void IsolateSelectedEntities(
			PulseForge::Scene* SceneValue,
			EditorSceneWorkspaceState& Workspace,
			bool RuntimeActive,
			EditorSceneVisibilityActions& Actions) const;
		void RestoreIsolatedVisibility(
			PulseForge::Scene* SceneValue,
			EditorSceneWorkspaceState& Workspace,
			bool RuntimeActive,
			EditorSceneVisibilityActions& Actions) const;
	};
}
