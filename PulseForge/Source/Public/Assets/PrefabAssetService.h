#pragma once

#include "Assets/AssetRegistry.h"
#include "Core/Core.h"
#include "Scene/Entity.h"
#include "Scene/Scene.h"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>

namespace PulseForge
{
	enum class PrefabAssetErrorCode : uint8_t
	{
		AssetNotFound,
		UnsupportedAssetType,
		PathResolutionFailed,
		SerializationFailed
	};

	struct PrefabAssetError
	{
		PrefabAssetErrorCode Code;
		AssetID Asset;
		std::string Message;
	};

	class PULSEFORGE_API PrefabAssetService final
	{
	public:
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
