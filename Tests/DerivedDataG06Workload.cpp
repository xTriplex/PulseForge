#include "TestSupport.h"

#include "Assets/AssetMetadata.h"
#include "Assets/AssetRegistry.h"
#include "Assets/DerivedDataArtifactStore.h"
#include "Assets/Detail/AssetCpuProducts.h"
#include "Renderer/EnvironmentLightingCache.h"
#include "Scene/UUID.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <string_view>
#include <vector>

namespace
{
	using namespace PulseForge;
	using Clock = std::chrono::steady_clock;

	struct WorkloadFiles
	{
		std::filesystem::path Source;
		std::filesystem::path ProjectRelative;
	};

	class TemporaryWorkload final
	{
	public:
		TemporaryWorkload()
		{
			std::error_code Error;
			const auto Temp = std::filesystem::temp_directory_path(Error);
			if (Error)
				return;
			const auto CanonicalTemp = std::filesystem::canonical(Temp, Error);
			const auto ID = UUID::Generate();
			if (Error || !ID)
				return;
			Root = CanonicalTemp / ("PulseForgeG06PbrWorkload-" + ID->ToString());
			ProjectRoot = Root / "Project";
			CacheRoot = Root / "Cache";
			std::filesystem::create_directories(ProjectRoot / "Assets", Error);
			if (Error)
				Root.clear();
		}

		~TemporaryWorkload()
		{
			if (Root.empty())
				return;
			std::error_code Error;
			std::filesystem::remove_all(Root, Error);
		}

		std::filesystem::path Root;
		std::filesystem::path ProjectRoot;
		std::filesystem::path CacheRoot;
	};

	constexpr uint64_t MaxSourceFileBytes = 2ull * 1024ull * 1024ull;
	constexpr uint64_t MaxCopiedSourceBytes = 4ull * 1024ull * 1024ull;

	const std::array<WorkloadFiles, 14> PbrWorkloadFiles = {
		WorkloadFiles{ "Assets/Models/PbrTestCube.gltf", "Assets/Models/PbrTestCube.gltf" },
		WorkloadFiles{ "Assets/Models/PbrTestCube.gltf.meta", "Assets/Models/PbrTestCube.gltf.meta" },
		WorkloadFiles{ "Assets/Environments/studio_small_01_1k.hdr", "Assets/Environments/studio_small_01_1k.hdr" },
		WorkloadFiles{ "Assets/Environments/studio_small_01_1k.hdr.meta", "Assets/Environments/studio_small_01_1k.hdr.meta" },
		WorkloadFiles{ "Assets/Textures/AlbedoTiles.ppm", "Assets/Textures/AlbedoTiles.ppm" },
		WorkloadFiles{ "Assets/Textures/AlbedoTiles.ppm.meta", "Assets/Textures/AlbedoTiles.ppm.meta" },
		WorkloadFiles{ "Assets/Textures/EmissiveStripes.ppm", "Assets/Textures/EmissiveStripes.ppm" },
		WorkloadFiles{ "Assets/Textures/EmissiveStripes.ppm.meta", "Assets/Textures/EmissiveStripes.ppm.meta" },
		WorkloadFiles{ "Assets/Textures/MetallicRoughness.ppm", "Assets/Textures/MetallicRoughness.ppm" },
		WorkloadFiles{ "Assets/Textures/MetallicRoughness.ppm.meta", "Assets/Textures/MetallicRoughness.ppm.meta" },
		WorkloadFiles{ "Assets/Textures/NormalBumps.ppm", "Assets/Textures/NormalBumps.ppm" },
		WorkloadFiles{ "Assets/Textures/NormalBumps.ppm.meta", "Assets/Textures/NormalBumps.ppm.meta" },
		WorkloadFiles{ "Assets/Textures/OcclusionGrid.ppm", "Assets/Textures/OcclusionGrid.ppm" },
		WorkloadFiles{ "Assets/Textures/OcclusionGrid.ppm.meta", "Assets/Textures/OcclusionGrid.ppm.meta" }
	};

	std::optional<AssetRecord> FindAsset(const AssetRegistry& Registry, const std::filesystem::path& RelativePath)
	{
		for (const AssetRecord& Record : Registry.GetAssets())
			if (Record.ProjectRelativePath == RelativePath)
				return Record;
		return std::nullopt;
	}

	std::expected<AssetRecord, std::string> RequireAsset(const AssetRegistry& Registry, const std::filesystem::path& Path)
	{
		if (auto Record = FindAsset(Registry, Path))
			return *Record;
		return std::unexpected("PBR workload asset is missing from the rebuilt registry: " + Path.generic_string());
	}

	uint64_t TexturePayloadBytes(const Detail::PreparedTextureCpuProduct& Product)
	{
		uint64_t Bytes = 0;
		for (const Detail::PreparedMipLevel& Mip : Product.MipLevels)
			Bytes += Mip.Pixels.size();
		return Bytes;
	}

	uint64_t EnvironmentPayloadBytes(const EnvironmentLightingData& Product)
	{
		uint64_t Bytes = Product.BrdfIntegrationLut.RGBA32FPixels.size() * sizeof(float);
		const auto AddCube = [&Bytes](const FloatCubeLevel& Level)
		{
			for (const auto& Face : Level.Faces)
				Bytes += Face.size() * sizeof(float);
		};
		AddCube(Product.Environment);
		AddCube(Product.DiffuseIrradiance);
		for (const FloatCubeLevel& Level : Product.PrefilteredSpecular)
			AddCube(Level);
		return Bytes;
	}

	uint64_t CacheBytes(const std::filesystem::path& Root)
	{
		std::error_code Error;
		if (!std::filesystem::is_directory(Root, Error) || Error)
			return 0;
		uint64_t Total = 0;
		for (std::filesystem::recursive_directory_iterator It(Root, std::filesystem::directory_options::none, Error), End;
			It != End && !Error; It.increment(Error))
		{
			const auto Status = It->symlink_status(Error);
			if (Error)
				return 0;
			if (std::filesystem::is_regular_file(Status))
			{
				const uint64_t Size = It->file_size(Error);
				if (Error || Size > (std::numeric_limits<uint64_t>::max)() - Total)
					return 0;
				Total += Size;
			}
		}
		return Error ? 0 : Total;
	}

	std::string_view ColorSpaceFor(const std::filesystem::path& Path)
	{
		const auto Name = Path.filename().string();
		return Name == "AlbedoTiles.ppm" || Name == "EmissiveStripes.ppm" ? "srgb" : "linear";
	}

	struct TextureMeasurement
	{
		AssetRecord Asset;
		Detail::ImageColorSpace ColorSpace;
	};

	std::vector<TextureMeasurement> FindPbrTextures(const AssetRegistry& Registry)
	{
		std::vector<TextureMeasurement> Result;
		for (const std::string_view Name : { "AlbedoTiles.ppm", "EmissiveStripes.ppm", "MetallicRoughness.ppm",
			"NormalBumps.ppm", "OcclusionGrid.ppm" })
		{
			const std::filesystem::path Path = std::filesystem::path("Assets") / "Textures" / Name;
			if (auto Record = FindAsset(Registry, Path))
				Result.push_back({ *Record, ColorSpaceFor(Path) == "srgb" ? Detail::ImageColorSpace::Srgb : Detail::ImageColorSpace::Linear });
		}
		return Result;
	}

	void RunProfile(TestRunner& Tests, const std::filesystem::path& SourceProjectRoot)
	{
		TemporaryWorkload Workload;
		PF_CHECK(Tests, !Workload.Root.empty());
		if (Workload.Root.empty())
			return;

		uint64_t CopiedBytes = 0;
		for (const WorkloadFiles& File : PbrWorkloadFiles)
		{
			const auto Source = SourceProjectRoot / File.Source;
			std::error_code Error;
			const uint64_t Size = std::filesystem::file_size(Source, Error);
			PF_CHECK(Tests, !Error && Size <= MaxSourceFileBytes && Size <= MaxCopiedSourceBytes - CopiedBytes);
			if (Error || Size > MaxSourceFileBytes || Size > MaxCopiedSourceBytes - CopiedBytes)
				return;
			CopiedBytes += Size;
			const auto Destination = Workload.ProjectRoot / File.ProjectRelative;
			std::filesystem::create_directories(Destination.parent_path(), Error);
			if (!Error)
				std::filesystem::copy_file(Source, Destination, std::filesystem::copy_options::none, Error);
			PF_CHECK(Tests, !Error);
			if (Error)
				return;
		}

		AssetRegistry Registry;
		const auto Rebuilt = Registry.Rebuild(Workload.ProjectRoot);
		PF_CHECK(Tests, Rebuilt.has_value() && Registry.GetAssetCount() == 7);
		if (!Rebuilt || Registry.GetAssetCount() != 7)
			return;
		const auto Mesh = RequireAsset(Registry, "Assets/Models/PbrTestCube.gltf");
		const auto Environment = RequireAsset(Registry, "Assets/Environments/studio_small_01_1k.hdr");
		const auto Textures = FindPbrTextures(Registry);
		PF_CHECK(Tests, Mesh.has_value() && Environment.has_value() && Textures.size() == 5);
		if (!Mesh || !Environment || Textures.size() != 5)
			return;

		const auto MeshMetadata = AssetMetadataSerializer::LoadFromFile(
			AssetMetadataSerializer::GetSidecarPath(Workload.ProjectRoot / Mesh->ProjectRelativePath));
		PF_CHECK(Tests, MeshMetadata.has_value());
		if (!MeshMetadata)
			return;
		const auto Settings = GltfMeshImportSettings::Parse(MeshMetadata->ImportSettingsJson);
		const auto EffectiveSettings = Settings ? Settings->Canonicalize() : std::expected<std::string, std::string>(std::unexpected("invalid settings"));
		PF_CHECK(Tests, EffectiveSettings.has_value());
		if (!EffectiveSettings)
			return;

		const DerivedDataArtifactStoreOptions StoreOptionsTemplate{
			.Root = {}, .Enabled = true, .MaxTotalBytes = 256ull * 1024ull * 1024ull,
			.MaxArtifactBytes = 64ull * 1024ull * 1024ull, .MaxEntries = 32
		};
		DerivedDataArtifactStoreOptions ColdOptions = StoreOptionsTemplate;
		ColdOptions.Root = Workload.CacheRoot;
		DerivedDataArtifactStore ColdStore(ColdOptions);

		const auto ColdStart = Clock::now();
		auto ColdMesh = Detail::PrepareMeshCpuProduct(Mesh->ID, Workload.ProjectRoot, Registry, *EffectiveSettings, &ColdStore);
		PF_CHECK(Tests, ColdMesh && ColdMesh->CacheOutcome == Detail::AssetCpuProductCacheOutcome::ProcessedAndStored);
		if (!ColdMesh)
			return;
		uint64_t ColdTexturePayload = 0;
		uint64_t ColdTextureLookup = 0;
		uint64_t ColdTextureProcess = 0;
		uint64_t ColdTextureWrite = 0;
		for (const TextureMeasurement& Texture : Textures)
		{
			auto Prepared = Detail::PrepareTextureCpuProduct(Texture.Asset.ID, Workload.ProjectRoot, Registry,
				Texture.ColorSpace, &ColdStore);
			PF_CHECK(Tests, Prepared && Prepared->CacheOutcome == Detail::AssetCpuProductCacheOutcome::ProcessedAndStored);
			if (!Prepared)
				return;
			ColdTexturePayload += TexturePayloadBytes(Prepared->Product);
			ColdTextureLookup += Prepared->ObservationAndLookupMicroseconds;
			ColdTextureProcess += Prepared->ProcessingMicroseconds;
			ColdTextureWrite += Prepared->CacheWriteMicroseconds;
		}
		auto ColdEnvironment = Detail::PrepareEnvironmentCpuProduct(
			Environment->ID, Workload.ProjectRoot, Registry, &ColdStore, EnvironmentProcessingDesc{});
		PF_CHECK(Tests, ColdEnvironment && ColdEnvironment->RanEnvironmentPreprocessing && ColdEnvironment->RanBrdfPreprocessing);
		if (!ColdEnvironment)
			return;
		const auto ColdEnd = Clock::now();
		const uint64_t ColdDiskBytes = CacheBytes(Workload.CacheRoot);

		DerivedDataArtifactStore WarmStore({
			.Root = Workload.CacheRoot, .Enabled = true, .MaxTotalBytes = StoreOptionsTemplate.MaxTotalBytes,
			.MaxArtifactBytes = StoreOptionsTemplate.MaxArtifactBytes, .MaxEntries = StoreOptionsTemplate.MaxEntries
		});
		const auto WarmStart = Clock::now();
		auto WarmMesh = Detail::PrepareMeshCpuProduct(Mesh->ID, Workload.ProjectRoot, Registry, *EffectiveSettings, &WarmStore);
		PF_CHECK(Tests, WarmMesh && WarmMesh->CacheOutcome == Detail::AssetCpuProductCacheOutcome::ArtifactHit);
		if (!WarmMesh)
			return;
		uint64_t WarmTexturePayload = 0;
		uint64_t WarmTextureLookup = 0;
		for (const TextureMeasurement& Texture : Textures)
		{
			auto Prepared = Detail::PrepareTextureCpuProduct(Texture.Asset.ID, Workload.ProjectRoot, Registry,
				Texture.ColorSpace, &WarmStore);
			PF_CHECK(Tests, Prepared && Prepared->CacheOutcome == Detail::AssetCpuProductCacheOutcome::ArtifactHit);
			if (!Prepared)
				return;
			WarmTexturePayload += TexturePayloadBytes(Prepared->Product);
			WarmTextureLookup += Prepared->ObservationAndLookupMicroseconds;
		}
		auto WarmEnvironment = Detail::PrepareEnvironmentCpuProduct(
			Environment->ID, Workload.ProjectRoot, Registry, &WarmStore, EnvironmentProcessingDesc{});
		PF_CHECK(Tests, WarmEnvironment && WarmEnvironment->EnvironmentArtifactHit && WarmEnvironment->BrdfArtifactHit &&
			!WarmEnvironment->RanEnvironmentPreprocessing && !WarmEnvironment->RanBrdfPreprocessing);
		if (!WarmEnvironment)
			return;
		const auto WarmEnd = Clock::now();
		const uint64_t WarmDiskBytes = CacheBytes(Workload.CacheRoot);

		const auto Micros = [](Clock::duration Duration)
		{
			return std::chrono::duration_cast<std::chrono::microseconds>(Duration).count();
		};
		const auto PhaseMicros = [](uint64_t Start, uint64_t End)
		{
			return Start != 0 && End >= Start ? (End - Start) / 1000 : 0;
		};
		const uint64_t MeshBytes = ColdMesh->Product.Vertices.size() * sizeof(GltfMeshVertex) +
			ColdMesh->Product.Indices.size() * sizeof(uint32_t);
		const uint64_t ColdEnvironmentPayload = EnvironmentPayloadBytes(ColdEnvironment->Data);
		const uint64_t WarmEnvironmentPayload = EnvironmentPayloadBytes(WarmEnvironment->Data);
		PF_CHECK(Tests, WarmMesh->Product.Indices == ColdMesh->Product.Indices &&
			WarmMesh->Product.Vertices.size() == ColdMesh->Product.Vertices.size());
		PF_CHECK(Tests, WarmTexturePayload == ColdTexturePayload && ColdEnvironmentPayload == WarmEnvironmentPayload);
		PF_CHECK(Tests, ColdDiskBytes > 0 && WarmDiskBytes == ColdDiskBytes && CopiedBytes <= MaxCopiedSourceBytes);
		std::cout << "G11_G06_PBR_COLD_WARM cold_wall_us=" << Micros(ColdEnd - ColdStart)
			<< " cold_mesh_lookup_us=" << ColdMesh->ObservationAndLookupMicroseconds
			<< " cold_mesh_process_us=" << ColdMesh->ProcessingMicroseconds
			<< " cold_mesh_write_us=" << ColdMesh->CacheWriteMicroseconds
			<< " cold_texture_lookup_us=" << ColdTextureLookup
			<< " cold_texture_decode_mips_us=" << ColdTextureProcess
			<< " cold_texture_write_us=" << ColdTextureWrite
			<< " cold_environment_observe_cache_us=" << PhaseMicros(ColdEnvironment->DerivedCacheReadStartTimestampNanoseconds,
				ColdEnvironment->DerivedCacheReadEndTimestampNanoseconds)
			<< " cold_environment_decode_us=" << PhaseMicros(ColdEnvironment->ImportStartTimestampNanoseconds,
				ColdEnvironment->ImportEndTimestampNanoseconds)
			<< " cold_environment_process_us=" << PhaseMicros(ColdEnvironment->PreprocessStartTimestampNanoseconds,
				ColdEnvironment->PreprocessEndTimestampNanoseconds)
			<< " warm_wall_us=" << Micros(WarmEnd - WarmStart)
			<< " warm_mesh_lookup_us=" << WarmMesh->ObservationAndLookupMicroseconds
			<< " warm_mesh_process_us=" << WarmMesh->ProcessingMicroseconds
			<< " warm_texture_lookup_us=" << WarmTextureLookup
			<< " warm_texture_process_us=0"
			<< " warm_environment_observe_cache_us=" << PhaseMicros(WarmEnvironment->DerivedCacheReadStartTimestampNanoseconds,
				WarmEnvironment->DerivedCacheReadEndTimestampNanoseconds)
			<< " warm_environment_process_us=0"
			<< " cpu_payload_bytes=" << MeshBytes + ColdTexturePayload + ColdEnvironmentPayload
			<< " cache_disk_bytes=" << ColdDiskBytes << " source_copy_bytes=" << CopiedBytes
			<< " processors_bypassed=true gpu_upload_measured=false\n";
	}
}

void RunG06RepresentativeWorkloadMeasurement(TestRunner& Tests)
{
	if (const char* ProjectRoot = std::getenv("PF_G11_G06_PROJECT_ROOT"); ProjectRoot && *ProjectRoot)
		RunProfile(Tests, ProjectRoot);
}
