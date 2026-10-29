#pragma once

#include "Assets/PrefabInstanceReconciler.h"
#include "Scene/Scene.h"

#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <vector>

namespace PulseForgeEditor
{
	class EditorSceneWorkspaceState;
	class EditorStyle;

	struct EditorPrefabInstancePanelStatus
	{
		PulseForge::AssetID SourceAsset;
		PulseForge::UUID InstanceID;
		size_t OverrideCount = 0;
		size_t ConflictCount = 0;
		bool HasSelectedSourceOverrides = false;
		bool HasPendingPlan = false;
		bool CanApply = false;
		PulseForge::PrefabReconcileSummary PlanSummary;
		std::vector<PulseForge::PrefabConflictRecord> Conflicts;
	};

	class EditorPrefabInstancePanelActions
	{
	public:
		virtual ~EditorPrefabInstancePanelActions() = default;

		[[nodiscard]] virtual std::expected<std::optional<EditorPrefabInstancePanelStatus>, std::string> GetStatus(
			const PulseForge::Entity& Entity) = 0;
		[[nodiscard]] virtual std::expected<EditorPrefabInstancePanelStatus, std::string> PlanUpdate(
			const PulseForge::Entity& Entity) = 0;
		[[nodiscard]] virtual std::expected<EditorPrefabInstancePanelStatus, std::string> ApplyUpdate(
			const PulseForge::Entity& Entity) = 0;
		[[nodiscard]] virtual std::expected<void, std::string> ApplyOverrides(
			const PulseForge::Entity& Entity,
			bool All) = 0;
		[[nodiscard]] virtual std::expected<void, std::string> RevertOverrides(
			const PulseForge::Entity& Entity,
			bool All) = 0;
	};

	class EditorPrefabInstancePanel final
	{
	public:
		void Draw(
			PulseForge::Scene* SceneValue,
			EditorSceneWorkspaceState& Workspace,
			EditorStyle& Style,
			EditorPrefabInstancePanelActions& Actions);

	private:
		void DrawConflicts(const std::vector<PulseForge::PrefabConflictRecord>& Conflicts) const;

		std::optional<PulseForge::UUID> m_ErrorEntity;
		std::string m_Error;
		std::string m_Status;
	};
} // namespace PulseForgeEditor
