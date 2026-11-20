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
		size_t GraphMaterialShaderResources = 0;
		size_t GraphMaterialIdentityDescribeCalls = 0;
		size_t GraphMaterialIdentityCacheHits = 0;
		uint64_t GraphMaterialCompileRequests = 0;
		uint64_t GraphMaterialCompileFailures = 0;
		uint64_t GraphMaterialDxcProcessLaunches = 0;
		uint64_t GraphMaterialShaderCreations = 0;
		uint64_t GraphMaterialBindingLayoutCreations = 0;
		uint64_t GraphMaterialParameterBufferCreations = 0;
		uint64_t GraphMaterialBindingSetCreations = 0;
		uint64_t GraphMaterialParameterUploads = 0;
		size_t GraphMaterialPipelines = 0;
		uint64_t GraphMaterialPipelineCreations = 0;
		uint64_t GraphMaterialPipelineEvictions = 0;
		size_t SkeletalMeshResources = 0;
		bool DeviceAllocationBytesAvailable = false;
	};
} // namespace PulseForge
