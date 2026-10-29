#pragma once

#include "Assets/AssetRegistry.h"
#include "Assets/PrefabInstanceReconciler.h"
#include "Core/Core.h"
#include "Scene/Entity.h"
#include "Scene/Scene.h"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace PulseForge
{
	enum class PrefabAssetErrorCode : uint8_t
	{
		AssetNotFound,
		SourceFileMissing,
		UnsupportedAssetType,
		PathResolutionFailed,
		SerializationFailed,
		AssetOperationFailed,
		CommittedWithCleanupFailure,
		ReconciliationFailed,
		StaleReconcilePlan,
		ReconcileConflicts,
		StaleSource
	};

	struct PrefabAssetError
	{
		PrefabAssetErrorCode Code;
		AssetID Asset;
		std::string Message;
		std::optional<AssetRecord> CommittedAsset;
		std::optional<std::filesystem::path> RecoveryPath;
		std::vector<PrefabConflictRecord> Conflicts;
	};

	class PULSEFORGE_API PrefabAssetService final
	{
	public:
		// Serializes a new prefab asset with a fresh sidecar UUID and adds it to the supplied project registry.
		// A CommittedWithCleanupFailure error carries the created record in CommittedAsset; do not retry it.
		[[nodiscard]] static std::expected<AssetRecord, PrefabAssetError> Create(
			AssetRegistry& Registry,
			const std::filesystem::path& ProjectRoot,
			const std::filesystem::path& ProjectRelativePath,
			const Scene& Source,
			const Entity& Root);

		// Instantiates the prefab found by sidecar UUID, assigning fresh entity UUIDs while retaining asset references.
		[[nodiscard]] static std::expected<Entity, PrefabAssetError> Instantiate(
			const AssetID& PrefabAsset,
			const std::filesystem::path& ProjectRoot,
			const AssetRegistry& Registry,
			Scene& Destination);

		// Loads an owned editable prefab source while retaining its embedded entity UUIDs.
		[[nodiscard]] static std::expected<PrefabSourceDocument, PrefabAssetError> LoadSource(
			const AssetID& PrefabAsset,
			const std::filesystem::path& ProjectRoot,
			const AssetRegistry& Registry);

		// Saves a source draft in place and refreshes its content fingerprint after a successful replacement.
		[[nodiscard]] static std::expected<void, PrefabAssetError> SaveSource(
			const AssetID& PrefabAsset,
			const std::filesystem::path& ProjectRoot,
			const AssetRegistry& Registry,
			PrefabSourceDocument& Source);

		// Creates a linked instance and persists its fresh InstanceID and source-to-live UUID map on the root.
		[[nodiscard]] static std::expected<Entity, PrefabAssetError> InstantiateLinked(
			const AssetID& PrefabAsset,
			const std::filesystem::path& ProjectRoot,
			const AssetRegistry& Registry,
			Scene& Destination,
			const PrefabPlacement& Placement = {});

		// Plans against the current source file; apply rechecks both its byte fingerprint and the serialized draft snapshot.
		[[nodiscard]] static std::expected<PrefabReconcilePlan, PrefabAssetError> PlanReconcile(
			const AssetID& PrefabAsset,
			const std::filesystem::path& ProjectRoot,
			const AssetRegistry& Registry,
			const Scene& Destination,
			const Entity& LinkedRoot);
		[[nodiscard]] static std::expected<PrefabReconcileSummary, PrefabAssetError> ApplyReconcile(
			const AssetID& PrefabAsset,
			const std::filesystem::path& ProjectRoot,
			const AssetRegistry& Registry,
			Scene& Destination,
			const PrefabReconcilePlan& Plan);

		// Saves to the existing managed prefab path without changing its sidecar UUID.
		[[nodiscard]] static std::expected<void, PrefabAssetError> Save(
			const AssetID& PrefabAsset,
			const std::filesystem::path& ProjectRoot,
			const AssetRegistry& Registry,
			const Scene& Source,
			const Entity& Root);
	};
}
