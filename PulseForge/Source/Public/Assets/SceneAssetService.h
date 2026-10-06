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
		SerializationFailed
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
