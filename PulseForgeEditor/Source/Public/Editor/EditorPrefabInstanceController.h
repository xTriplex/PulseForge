#pragma once

#include "Assets/PrefabAssetService.h"
#include "Editor/EditorInspectorPanel.h"
#include "Editor/EditorPrefabInstancePanel.h"

#include <expected>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace PulseForge
{
	class Project;
	class Scene;
}

namespace PulseForgeEditor
{
	class EditorDocumentManager;
	class EditorPrefabDocumentEditor;

	// Supplies only the project/scene boundary operations needed by linked-instance commands.
	class EditorPrefabInstanceControllerHost
	{
	public:
		virtual ~EditorPrefabInstanceControllerHost() = default;
		[[nodiscard]] virtual PulseForge::Project* GetPrefabProject() = 0;
		[[nodiscard]] virtual PulseForge::Scene* GetPrefabScene() const = 0;
		[[nodiscard]] virtual uint64_t GetPrefabSceneSessionGeneration() const = 0;
		[[nodiscard]] virtual bool IsPrefabRuntimeActive() const = 0;
		virtual void QueuePrefabOperation(std::function<void()> Operation) = 0;
		[[nodiscard]] virtual std::expected<void, std::string> RunPrefabSceneEdit(
			std::string Description, std::function<std::expected<void, PulseForge::SceneError>()> Operation) = 0;
		virtual void UpdatePrefabSceneDirty() = 0;
		[[nodiscard]] virtual EditorDocumentManager& GetPrefabDocumentManager() = 0;
		[[nodiscard]] virtual EditorPrefabDocumentEditor* GetPrefabDocumentEditor() const = 0;
		[[nodiscard]] virtual bool EnsurePrefabDocumentOpen(const PulseForge::AssetID& Asset) = 0;
		virtual void ReportPrefabError(std::string Message) = 0;
		virtual void ReportPrefabWarning(std::string Message) = 0;
		virtual void ReportPrefabStatus(std::string Message) = 0;
	};

	class EditorPrefabInstanceController final : public EditorPrefabInstancePanelActions
	{
	public:
		explicit EditorPrefabInstanceController(EditorPrefabInstanceControllerHost& Host) : m_Host(Host) {}

		[[nodiscard]] std::expected<std::optional<EditorPrefabInstancePanelStatus>, std::string> GetStatus(
			const PulseForge::Entity& Entity) override;
		[[nodiscard]] std::expected<EditorPrefabInstancePanelStatus, std::string> PlanUpdate(
			const PulseForge::Entity& Entity) override;
		[[nodiscard]] std::expected<EditorPrefabInstancePanelStatus, std::string> ApplyUpdate(
			const PulseForge::Entity& Entity) override;
		[[nodiscard]] std::expected<void, std::string> ApplyOverrides(const PulseForge::Entity& Entity, bool All) override;
		[[nodiscard]] std::expected<void, std::string> RevertOverrides(const PulseForge::Entity& Entity, bool All) override;

		[[nodiscard]] bool IsLinkedPrefabEntity(const PulseForge::Entity& Entity) const;
		[[nodiscard]] std::expected<void, std::string> RecordPropertyOverride(
			const PulseForge::Entity& Entity, std::string_view ComponentID, std::string_view PropertyID);
		[[nodiscard]] std::expected<void, std::string> RecordRenameOverride(
			const PulseForge::Entity& Entity, std::string_view Name);
		[[nodiscard]] std::expected<void, std::string> ResetPropertyOverride(
			const PulseForge::Entity& Entity, std::string_view ComponentID, std::string_view PropertyID);
		[[nodiscard]] std::expected<void, std::string> RecordReparent(
			const PulseForge::Entity& Entity, const std::optional<PulseForge::Entity>& Parent);
		[[nodiscard]] std::expected<void, std::string> RecordRemoval(const PulseForge::Entity& Entity);

	private:
		[[nodiscard]] std::expected<void, std::string> QueueOverrideOperation(
			const PulseForge::Entity& Entity, bool All, bool Revert);
		void RecordMissingSourceConflict(
			const PulseForge::UUID& RootID,
			const PulseForge::UUID& ProjectID,
			const PulseForge::PrefabInstanceComponent& ExpectedInstance,
			uint64_t SceneSessionGeneration,
			std::string Message);

		EditorPrefabInstanceControllerHost& m_Host;
		std::optional<PulseForge::PrefabReconcilePlan> m_PendingReconcile;
		std::optional<PulseForge::UUID> m_PendingProjectID;
		std::optional<uint64_t> m_PendingSceneSessionGeneration;
	};
} // namespace PulseForgeEditor
