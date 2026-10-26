#pragma once

#include "Assets/AssetRegistry.h"
#include "Assets/GltfMeshImporter.h"
#include "Assets/DerivedDataArtifactStore.h"
#include "Assets/AssetImportProvenance.h"
#include "Assets/CookedProductCodec.h"
#include "Assets/ImportedImageData.h"

#include <expected>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace PulseForge::Detail
{
	struct AssetCpuPhaseTiming
	{
		uint64_t StartTimestampNanoseconds = 0;
		uint64_t EndTimestampNanoseconds = 0;
	};

	enum class AssetCpuProductCacheOutcome : uint8_t
	{
		ArtifactHit,
		ProcessedAndStored,
		ProcessedStoreUnavailable
	};

	template<typename T>
	struct AssetCpuProductResult
	{
		T Product;
		AssetCpuProductCacheOutcome CacheOutcome = AssetCpuProductCacheOutcome::ProcessedStoreUnavailable;
		uint64_t ObservationAndLookupMicroseconds = 0;
		uint64_t ProcessingMicroseconds = 0;
		uint64_t CacheWriteMicroseconds = 0;
		AssetCpuPhaseTiming ObservationAndLookup;
		AssetCpuPhaseTiming Processing;
		AssetCpuPhaseTiming CacheWrite;
	};

	using MeshProcessor = std::function<std::expected<GltfMeshImportProduct, std::string>(
		const AssetID&, const std::filesystem::path&, const AssetRegistry&)>;
	using ImageProcessor = std::function<std::expected<ImportedImage, std::string>(
		const AssetID&, const std::filesystem::path&, const AssetRegistry&)>;

	[[nodiscard]] PULSEFORGE_API std::optional<DerivedDataArtifactKey> MakeMeshArtifactKey(
		const AssetImportRecipe& Recipe, const AssetID& Asset, std::string_view EffectiveSettings);
	[[nodiscard]] PULSEFORGE_API DerivedDataArtifactKey MakeTextureArtifactKey(
		const AssetContentFingerprint& Source, ImageColorSpace ColorSpace);
	[[nodiscard]] PULSEFORGE_API std::vector<PreparedMipLevel> BuildTextureMipChain(
		uint32_t Width, uint32_t Height, std::span<const uint8_t> SourcePixels, ImageColorSpace ColorSpace);
	// CPU-only preparation. A cache hit returns a validated owned product without calling the processor.
	// Processor overrides are per-call deterministic test seams; normal consumers use the production importer.
	[[nodiscard]] PULSEFORGE_API std::expected<AssetCpuProductResult<ImportedGltfMesh>, std::string> PrepareMeshCpuProduct(
		const AssetID& Asset,
		const std::filesystem::path& ProjectRoot,
		const AssetRegistry& Registry,
		std::string_view EffectiveSettings,
		DerivedDataArtifactStore* ArtifactStore,
		std::stop_token StopToken = {},
		MeshProcessor Processor = {});

	[[nodiscard]] PULSEFORGE_API std::expected<AssetCpuProductResult<PreparedTextureCpuProduct>, std::string> PrepareTextureCpuProduct(
		const AssetID& Asset,
		const std::filesystem::path& ProjectRoot,
		const AssetRegistry& Registry,
		ImageColorSpace ColorSpace,
		DerivedDataArtifactStore* ArtifactStore,
		std::stop_token StopToken = {},
		ImageProcessor Processor = {});
} // namespace PulseForge::Detail
