#pragma once

#include "Renderer/AssetCache/AssetCacheInventoryStats.h"
#include "Scene/UUID.h"

#include <cstddef>
#include <cstdint>

namespace PulseForge
{
	struct SceneRendererResourceCacheStats final
	{
		UUID ProjectID;
		uint64_t Generation = 0;
		AssetCacheInventoryStats Meshes;
		AssetCacheInventoryStats Textures;
		AssetCacheInventoryStats EnvironmentLighting;
		size_t MaterialDescriptions = 0;
		size_t GraphMaterialResources = 0;
		size_t SkeletalMeshResources = 0;
		bool DeviceAllocationBytesAvailable = false;
	};
} // namespace PulseForge
