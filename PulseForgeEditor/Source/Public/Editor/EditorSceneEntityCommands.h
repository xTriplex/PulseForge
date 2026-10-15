#pragma once

#include "Editor/EditorEntityClipboard.h"
#include "Editor/EditorSceneCommandActions.h"
#include "Assets/AssetID.h"
#include "Scene/Scene.h"
#include "Scene/UUID.h"

#include <functional>
#include <optional>

namespace PulseForgeEditor
{
	class EditorSceneWorkspaceState;

	class EditorSceneEntityCommands final
	{
	public:
		void CreateEntity(PulseForge::Scene* SceneValue, EditorSceneEntityCommandActions& Actions) const;
		void DuplicateSelectedEntities(
			PulseForge::Scene* SceneValue,
			EditorSceneWorkspaceState& Workspace,
			EditorSceneEntityCommandActions& Actions) const;
		void DeleteSelectedEntities(
			PulseForge::Scene* SceneValue,
			EditorSceneWorkspaceState& Workspace,
			EditorSceneEntityCommandActions& Actions) const;
		[[nodiscard]] bool CopySelectedEntities(
			PulseForge::Scene* SceneValue,
			const PulseForge::UUID& ProjectID,
			std::optional<PulseForge::AssetID> SceneAsset,
			EditorSceneWorkspaceState& Workspace,
			EditorSceneCommandActions& Actions);
		void CutSelectedEntities(
			PulseForge::Scene* SceneValue,
			const PulseForge::UUID& ProjectID,
			std::optional<PulseForge::AssetID> SceneAsset,
			EditorSceneWorkspaceState& Workspace,
			EditorSceneEntityCommandActions& Actions);
		void PasteEntities(
			PulseForge::Scene* SceneValue,
			const PulseForge::UUID& ProjectID,
			std::optional<PulseForge::AssetID> SceneAsset,
			EditorSceneWorkspaceState& Workspace,
			bool RuntimeActive,
			const std::function<bool(const PulseForge::UUID&)>& IsAssetRegistered,
			EditorSceneEntityCommandActions& Actions) const;
		void ReparentEntity(
			PulseForge::Scene* SceneValue,
			const PulseForge::UUID& ChildIdentifier,
			std::optional<PulseForge::UUID> ParentIdentifier,
			EditorSceneCommandActions& Actions) const;

		[[nodiscard]] bool CanPaste(const PulseForge::UUID& ProjectID) const noexcept;

	private:
		EditorEntityClipboard m_Clipboard;
	};
}
