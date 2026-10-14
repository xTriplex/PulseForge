#pragma once

#include "Editor/EditorIcons.h"
#include "Scene/Scene.h"

#include <optional>
#include <string>
#include <unordered_set>

namespace PulseForgeEditor
{
	class EditorSceneWorkspaceState;
	class EditorStyle;

	enum class EditorHierarchyPanelActionKind
	{
		CreateEntity,
		DuplicateSelection,
		DeleteSelection,
		CreatePrefab,
		ToggleVisibility,
		MakeRoot,
		ParentUnderSelection,
		ReportError
	};

	struct EditorHierarchyPanelAction
	{
		EditorHierarchyPanelActionKind Kind;
		PulseForge::UUID Entity;
		std::optional<PulseForge::UUID> Parent;
		std::string Message;
	};

	class EditorHierarchyPanelActionSink
	{
	public:
		virtual ~EditorHierarchyPanelActionSink() = default;
		virtual void HandleHierarchyPanelAction(const EditorHierarchyPanelAction& Action) = 0;
	};

	class EditorHierarchyPanel final
	{
	public:
		void FocusSearchNextFrame() noexcept { m_FocusSearchNextFrame = true; }
		void DrawContents(
			PulseForge::Scene* SceneValue,
			EditorSceneWorkspaceState& Workspace,
			EditorStyle& Style,
			EditorHierarchyPanelActionSink& Actions);

	private:
		void DrawEntityTree(
			const PulseForge::Entity& Entity,
			EditorSceneWorkspaceState& Workspace,
			EditorStyle& Style,
			EditorHierarchyPanelActionSink& Actions,
			std::optional<EditorHierarchyPanelAction>& DeferredAction,
			const std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash>* VisibleEntities);

		std::string m_Search;
		bool m_FocusSearchNextFrame = false;
	};

	[[nodiscard]] EditorIcon GetEditorEntityIcon(const PulseForge::Entity& Entity);
}
