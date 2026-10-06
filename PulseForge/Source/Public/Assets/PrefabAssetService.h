#pragma once

#include "Assets/AssetRegistry.h"
#include "Core/Core.h"
#include "Scene/Entity.h"
#include "Scene/Scene.h"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>

namespace PulseForge
{
	enum class PrefabAssetErrorCode : uint8_t
	{
		AssetNotFound,
		UnsupportedAssetType,
		PathResolutionFailed,
		SerializationFailed,
		AssetOperationFailed,
		CommittedWithCleanupFailure
	};

	struct PrefabAssetError
	{
		PrefabAssetErrorCode Code;
		AssetID Asset;
		std::string Message;
		std::optional<AssetRecord> CommittedAsset;
		std::optional<std::filesystem::path> RecoveryPath;
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

		// Saves to the existing managed prefab path without changing its sidecar UUID.
		[[nodiscard]] static std::expected<void, PrefabAssetError> Save(
			const AssetID& PrefabAsset,
			const std::filesystem::path& ProjectRoot,
			const AssetRegistry& Registry,
			const Scene& Source,
			const Entity& Root);
	};
}
