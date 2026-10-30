#pragma once

#include "Assets/PrefabSerializer.h"
#include "Core/Core.h"

#include <cstdint>
#include <cstddef>
#include <expected>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace PulseForge
{
	struct PrefabPlacement
	{
		// When omitted, the source root transform is used. This value is destination-owned after instantiation.
		std::optional<TransformComponent> RootTransform;
		std::optional<Entity> Parent;
	};

	struct PrefabReconcileSummary
	{
		size_t Added = 0;
		size_t ExistingEntitiesProcessed = 0;
		size_t Removed = 0;
		size_t ConflictCount = 0;
	};

	struct PrefabOverrideEditSummary
	{
		size_t Applied = 0;
		size_t RetainedInstanceLocal = 0;
	};

	enum class PrefabReconcileErrorCode : uint8_t
	{
		InvalidSource,
		InvalidInstance,
		SourceMismatch,
		StalePlan,
		ConflictsDetected,
		UnsupportedOverride,
		SceneOperationFailed,
		RecoveryRequired
	};

	struct PrefabReconcileError
	{
		PrefabReconcileErrorCode Code;
		std::string Message;
		std::vector<PrefabConflictRecord> Conflicts;
		// Serialized pre-operation scene. Set when an in-place commit cannot verify rollback.
		std::string RecoverySnapshot;
	};

	class PULSEFORGE_API PrefabReconcilePlan final
	{
	public:
		PrefabReconcilePlan(PrefabReconcilePlan&&) noexcept;
		PrefabReconcilePlan& operator=(PrefabReconcilePlan&&) noexcept;
		~PrefabReconcilePlan();
		PrefabReconcilePlan(const PrefabReconcilePlan&) = delete;
		PrefabReconcilePlan& operator=(const PrefabReconcilePlan&) = delete;

		[[nodiscard]] UUID GetInstanceID() const noexcept;
		[[nodiscard]] AssetID GetSourceAsset() const noexcept;
		[[nodiscard]] std::string_view GetSourceFingerprint() const noexcept;
		[[nodiscard]] std::string_view GetSourceSnapshotFingerprint() const noexcept;
		[[nodiscard]] const PrefabReconcileSummary& GetSummary() const noexcept;
		[[nodiscard]] const std::vector<PrefabConflictRecord>& GetConflicts() const noexcept;
		[[nodiscard]] bool CanApply() const noexcept;

	private:
		friend class PrefabInstanceReconciler;
		friend class PrefabAssetService;
		struct Detail;
		explicit PrefabReconcilePlan(std::unique_ptr<Detail> Data);
		std::unique_ptr<Detail> m_Data;
	};

	class PULSEFORGE_API PrefabInstanceReconciler final
	{
	public:
#if defined(PF_DEBUG)
		enum class CommitFailurePoint : uint8_t
		{
			AfterAdditionCreated,
			AfterExistingEntitySynchronized,
			AfterHierarchyTransition,
			BeforeRemoval,
			DuringRollbackRestore
		};
		static void SetCommitFailureSequenceForTesting(std::initializer_list<CommitFailurePoint> Sequence);
		static void ClearCommitFailureSequenceForTesting() noexcept;
#endif

		[[nodiscard]] static std::expected<PrefabInstantiationResult, PrefabReconcileError> InstantiateLinked(
			const PrefabSourceDocument& Source,
			Scene& Destination,
			const PrefabPlacement& Placement = {});

		[[nodiscard]] static std::expected<PrefabReconcilePlan, PrefabReconcileError> PlanReconcile(
			const Scene& Destination,
			const Entity& LinkedRoot,
			const PrefabSourceDocument& Source);
		[[nodiscard]] static std::expected<PrefabReconcileSummary, PrefabReconcileError> ApplyReconcile(
			Scene& Destination,
			const PrefabSourceDocument& Source,
			const PrefabReconcilePlan& Plan);
		// Applies mapped instance intent to the owned prefab source draft. Instance metadata is left intact until the
		// caller explicitly resets it; local-only entities and local-parent reparenting remain instance-owned.
		[[nodiscard]] static std::expected<PrefabOverrideEditSummary, PrefabReconcileError> ApplyOverridesToSource(
			PrefabSourceDocument& Source,
			const PrefabInstanceComponent& Instance,
			std::optional<PrefabEntityAddress> OnlyEntity = std::nullopt);
		// Restores selected or all mapped override operations from the supplied source snapshot, preserving root placement,
		// local entities and unrelated scene entities. A removed mapped entity receives a fresh live UUID when restored.
		[[nodiscard]] static std::expected<PrefabOverrideEditSummary, PrefabReconcileError> RevertOverrides(
			Scene& Destination,
			const Entity& LinkedRoot,
			const PrefabSourceDocument& Source,
			std::optional<PrefabEntityAddress> OnlyEntity = std::nullopt);
		[[nodiscard]] static std::expected<std::optional<Entity>, PrefabReconcileError> FindLinkedRoot(
			const Scene& Destination,
			const Entity& Target);
		[[nodiscard]] static std::expected<PrefabEntityAddress, PrefabReconcileError> ResolveOrRegisterEntityAddress(
			const Entity& LinkedRoot,
			const Entity& Target);
		[[nodiscard]] static std::expected<void, PrefabReconcileError> RecordPropertyOverride(
			const Entity& LinkedRoot,
			const Entity& Target,
			std::string ComponentID,
			std::string PropertyID,
			PropertyValue Value);
		[[nodiscard]] static std::expected<void, PrefabReconcileError> RecordRenameOverride(
			const Entity& LinkedRoot,
			const Entity& Target,
			std::string Name);
		[[nodiscard]] static std::expected<void, PrefabReconcileError> RecordReparentOverride(
			const Entity& LinkedRoot,
			const Entity& Target,
			const std::optional<Entity>& Parent);
		[[nodiscard]] static std::expected<void, PrefabReconcileError> RecordRemoveSourceEntityOverride(
			const Entity& LinkedRoot,
			const Entity& Target);
		[[nodiscard]] static std::expected<void, PrefabReconcileError> DiscardOverride(
			const Entity& LinkedRoot,
			PrefabEntityAddress EntityAddress,
			std::optional<std::pair<std::string_view, std::string_view>> Property = std::nullopt);
		[[nodiscard]] static std::expected<void, PrefabReconcileError> RecordMissingSourceAssetConflict(
			const Entity& LinkedRoot,
			std::string Message);

		[[nodiscard]] static std::expected<void, PrefabReconcileError> RecordOverride(
			const Entity& LinkedRoot,
			const PrefabOverrideRecord& Override);
		[[nodiscard]] static std::expected<void, PrefabReconcileError> ResetOverride(
			const Entity& LinkedRoot,
			const PrefabSourceDocument& Source,
			PrefabEntityAddress EntityAddress,
			std::string_view ComponentID,
			std::string_view PropertyID);
	};
} // namespace PulseForge
