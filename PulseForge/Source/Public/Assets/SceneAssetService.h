#pragma once

#include "Assets/AssetRegistry.h"
#include "Core/Core.h"
#include "Scene/Scene.h"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>

namespace PulseForge
{
	enum class SceneAssetErrorCode : uint8_t
	{
		AssetNotFound,
		UnsupportedAssetType,
		PathResolutionFailed,
		SerializationFailed,
		AssetOperationFailed
	};

	struct SceneAssetError
	{
		SceneAssetErrorCode Code;
		AssetID Asset;
		std::string Message;
	};

	class PULSEFORGE_API SceneAssetService final
	{
	public:
		// Serializes a new scene asset with a fresh sidecar UUID and adds it to the supplied project registry.
		[[nodiscard]] static std::expected<AssetRecord, SceneAssetError> Create(
			AssetRegistry& Registry,
			const std::filesystem::path& ProjectRoot,
			const std::filesystem::path& ProjectRelativePath,
			const Scene& Source);

		// Resolves the scene's current source path by sidecar UUID. Failed loads leave the destination scene unchanged.
		[[nodiscard]] static std::expected<void, SceneAssetError> Load(
			const AssetID& SceneAsset,
			const std::filesystem::path& ProjectRoot,
			const AssetRegistry& Registry,
			Scene& Destination);

		// Saves to the existing managed scene path without changing its sidecar UUID.
		[[nodiscard]] static std::expected<void, SceneAssetError> Save(
			const AssetID& SceneAsset,
			const std::filesystem::path& ProjectRoot,
			const AssetRegistry& Registry,
			const Scene& Source);
	};
}
