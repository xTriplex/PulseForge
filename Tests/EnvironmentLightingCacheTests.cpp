#include "TestSupport.h"

#include "Assets/AssetMetadata.h"
#include "Assets/AssetPreparationService.h"
#include "Assets/AssetRegistry.h"
#include "Assets/DerivedDataArtifactStore.h"
#include "Renderer/EnvironmentLightingCache.h"
#include "Scene/UUID.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace
{
	class TemporaryEnvironmentCacheRoot final
	{
	public:
		TemporaryEnvironmentCacheRoot()
		{
			const auto Id = PulseForge::UUID::Generate();
			if (Id)
				Path = std::filesystem::temp_directory_path() / ("PulseForgeEnvironmentArtifactTest-" + Id->ToString());
		}

		~TemporaryEnvironmentCacheRoot()
		{
			std::error_code Error;
			std::filesystem::remove_all(Path, Error);
		}

		std::filesystem::path Path;
	};

	PulseForge::EnvironmentProcessingDesc MakeSmallDescription()
	{
		PulseForge::EnvironmentProcessingDesc Description;
		Description.EnvironmentFaceSize = 2;
		Description.IrradianceFaceSize = 1;
		Description.SpecularFaceSize = 2;
		Description.BrdfLutSize = 2;
		Description.IrradianceSamples = 2;
		Description.PrefilterSamples = 2;
		Description.BrdfSamples = 2;
		return Description;
	}

	PulseForge::FloatCubeLevel MakeLevel(uint32_t Size, float Seed)
	{
		PulseForge::FloatCubeLevel Level;
		Level.Size = Size;
		const size_t FloatCount = static_cast<size_t>(Size) * Size * 4;
		for (size_t Face = 0; Face < Level.Faces.size(); ++Face)
		{
			Level.Faces[Face].resize(FloatCount);
			for (size_t Index = 0; Index < FloatCount; ++Index)
				Level.Faces[Face][Index] = Seed + static_cast<float>(Face * 10 + Index) * 0.01f;
		}
		return Level;
	}

	PulseForge::EnvironmentLightingData MakeData(const PulseForge::EnvironmentProcessingDesc& Description)
	{
		PulseForge::EnvironmentLightingData Data;
		Data.Environment = MakeLevel(Description.EnvironmentFaceSize, 1.0f);
		Data.DiffuseIrradiance = MakeLevel(Description.IrradianceFaceSize, 2.0f);
		uint32_t MipSize = Description.SpecularFaceSize;
		while (true)
		{
			Data.PrefilteredSpecular.push_back(MakeLevel(MipSize, 3.0f));
			if (MipSize == 1)
				break;
			MipSize = std::max(1u, MipSize / 2);
		}
		return Data;
	}

	std::vector<std::byte> Bytes(std::string_view Text)
	{
		const auto View = std::as_bytes(std::span(Text.data(), Text.size()));
		return { View.begin(), View.end() };
	}

	bool WriteBytes(const std::filesystem::path& Path, std::span<const std::byte> Data)
	{
		std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
		if (!Output)
			return false;
		Output.write(reinterpret_cast<const char*>(Data.data()), static_cast<std::streamsize>(Data.size()));
		return static_cast<bool>(Output);
	}

	bool SameEnvironment(const PulseForge::EnvironmentLightingData& Left, const PulseForge::EnvironmentLightingData& Right)
	{
		return Left.Environment.Size == Right.Environment.Size &&
			Left.Environment.Faces == Right.Environment.Faces &&
			Left.DiffuseIrradiance.Size == Right.DiffuseIrradiance.Size &&
			Left.DiffuseIrradiance.Faces == Right.DiffuseIrradiance.Faces &&
			Left.PrefilteredSpecular.size() == Right.PrefilteredSpecular.size() &&
			std::equal(Left.PrefilteredSpecular.begin(), Left.PrefilteredSpecular.end(), Right.PrefilteredSpecular.begin(),
				[](const auto& A, const auto& B) { return A.Size == B.Size && A.Faces == B.Faces; }) &&
			Left.BrdfIntegrationLut.Width == Right.BrdfIntegrationLut.Width &&
			Left.BrdfIntegrationLut.Height == Right.BrdfIntegrationLut.Height &&
			Left.BrdfIntegrationLut.RGBA32FPixels == Right.BrdfIntegrationLut.RGBA32FPixels;
	}

	std::vector<std::byte> MakeSmallRadianceHdr()
	{
		std::vector<std::byte> Bytes;
		const std::string Header = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 2 +X 2\n";
		const auto HeaderBytes = std::as_bytes(std::span(Header.data(), Header.size()));
		Bytes.insert(Bytes.end(), HeaderBytes.begin(), HeaderBytes.end());
		// Widths below the Radiance scanline-RLE minimum use four-byte RGBE pixels.
		const std::array<std::array<uint8_t, 4>, 4> Pixels{
			std::array<uint8_t, 4>{ 128, 64, 32, 129 }, { 64, 128, 32, 129 },
			{ 32, 64, 128, 129 }, { 128, 128, 128, 130 } };
		for (const std::array<uint8_t, 4>& Pixel : Pixels)
			for (uint8_t Channel : Pixel)
				Bytes.push_back(static_cast<std::byte>(Channel));
		return Bytes;
	}

	PulseForge::AssetPreparationServiceLimits MakeTestPreparationLimits()
	{
		PulseForge::AssetPreparationServiceLimits Limits;
		Limits.MaximumWorkers = 1;
		Limits.MaximumOutstandingJobs = 2;
		Limits.MaximumSubscribers = 2;
		Limits.MaximumReservedWorkingBytes = 8 * 1024 * 1024;
		Limits.MaximumCompletedPayloadBytes = 8 * 1024 * 1024;
		return Limits;
	}

	std::optional<PulseForge::Detail::EnvironmentCpuPreparationResult> RunCpuPreparation(
		PulseForge::AssetPreparationService& Service,
		const std::string& Session,
		const PulseForge::AssetID& Asset,
		const std::filesystem::path& ProjectRoot,
		PulseForge::AssetRegistry Registry,
		PulseForge::DerivedDataArtifactStore& Store,
		const PulseForge::EnvironmentProcessingDesc& Processing)
	{
		using namespace PulseForge;
		const AssetPreparationRequestIdentity Identity{
			Session,
			"environment-cpu-test/" + Asset.ToString() + "/" + std::to_string(Processing.EnvironmentFaceSize) + "/" +
				std::to_string(Processing.IrradianceFaceSize) + "/" + std::to_string(Processing.SpecularFaceSize) + "/" +
				std::to_string(Processing.BrdfLutSize) + "/" + std::to_string(Processing.IrradianceSamples) + "/" +
				std::to_string(Processing.PrefilterSamples) + "/" + std::to_string(Processing.BrdfSamples) };
		auto Requested = Service.Request(Identity, AssetPreparationPriority::Interactive, 4 * 1024 * 1024,
			[Asset, ProjectRoot, Registry = std::move(Registry), StorePtr = &Store, Processing](std::stop_token Token) mutable
				-> std::expected<AssetPreparationService::CpuPayload, AssetPreparationError>
			{
				auto Product = Detail::PrepareEnvironmentCpuProduct(
					Asset, ProjectRoot, std::move(Registry), StorePtr, Processing, Token);
				if (!Product)
					return std::unexpected(std::move(Product.error()));
				const size_t RetainedBytes = Product->Data.BrdfIntegrationLut.RGBA32FPixels.size() * sizeof(float);
				return AssetPreparationService::CpuPayload(
					MakeAssetPreparationValue(std::move(*Product), RetainedBytes));
			});
		if (!Requested)
			return std::nullopt;

		const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (std::chrono::steady_clock::now() < Deadline)
		{
			const AssetPreparationSnapshot Snapshot = Requested->Poll();
			if (Snapshot.Status == AssetPreparationStatus::CpuReady)
			{
				const auto* Value = dynamic_cast<const AssetPreparationValue<Detail::EnvironmentCpuPreparationResult>*>(
					Snapshot.CpuPayload.get());
				if (Value)
					return Value->Get();
				return std::nullopt;
			}
			if (Snapshot.Status == AssetPreparationStatus::Failed || Snapshot.Status == AssetPreparationStatus::Cancelled)
				return std::nullopt;
			std::this_thread::yield();
		}
		Requested->Cancel();
		return std::nullopt;
	}
}

void RunEnvironmentLightingCacheTests(TestRunner& Tests)
{
	using namespace PulseForge;
	TemporaryEnvironmentCacheRoot Root;
	PF_CHECK(Tests, !Root.Path.empty());
	const EnvironmentProcessingDesc Description = MakeSmallDescription();
	const EnvironmentLightingData Original = MakeData(Description);
	const auto Encoded = Detail::EnvironmentLightingArtifactCodec::EncodeEnvironment(Original, Description);
	PF_CHECK(Tests, Encoded.has_value());
	if (!Encoded)
		return;

	const auto Asset = UUID::Generate();
	PF_CHECK(Tests, Asset.has_value());
	if (!Asset)
		return;
	const auto SourceBytes = Bytes("environment source bytes");
	const AssetContentFingerprint SourceFingerprint = ComputeAssetContentFingerprint(SourceBytes);
	const DerivedDataArtifactKey Key = Detail::EnvironmentLightingArtifactCodec::MakeEnvironmentKey(
		*Asset, SourceFingerprint, Description);
	const DerivedDataArtifactStoreOptions Options{ Root.Path, true, 1024 * 1024, 1024 * 1024, 16 };
	DerivedDataArtifactStore ColdStore(Options);
	PF_CHECK(Tests, ColdStore.Read(Key).Status == DerivedDataArtifactReadStatus::Miss);
	PF_CHECK(Tests, ColdStore.Write(Key, *Encoded).Status == DerivedDataArtifactWriteStatus::Written);

	// A new store instance proves the warm result comes from disk, not an in-memory cache.
	DerivedDataArtifactStore WarmStore(Options);
	const DerivedDataArtifactReadResult Warm = WarmStore.Read(Key);
	PF_CHECK(Tests, Warm.Status == DerivedDataArtifactReadStatus::Hit);
	const auto Decoded = Detail::EnvironmentLightingArtifactCodec::DecodeEnvironment(Warm.Payload, Description);
	PF_CHECK(Tests, Decoded.has_value());
	if (Decoded)
	{
		PF_CHECK(Tests, SameEnvironment(Original, *Decoded));
	}
	PF_CHECK(Tests, WarmStore.Read(Key).Payload == Warm.Payload);

	const DerivedDataArtifactKey ChangedSource = Detail::EnvironmentLightingArtifactCodec::MakeEnvironmentKey(
		*Asset, ComputeAssetContentFingerprint(Bytes("different environment source")), Description);
	EnvironmentProcessingDesc ChangedDescription = Description;
	ChangedDescription.PrefilterSamples++;
	const DerivedDataArtifactKey ChangedSettings = Detail::EnvironmentLightingArtifactCodec::MakeEnvironmentKey(
		*Asset, SourceFingerprint, ChangedDescription);
	PF_CHECK(Tests, ChangedSource.RecipeDigest != Key.RecipeDigest);
	PF_CHECK(Tests, ChangedSettings.RecipeDigest != Key.RecipeDigest);
	PF_CHECK(Tests, ChangedSource.ProductKind == Key.ProductKind);

	// Corrupt entries are classified by the production store and the product decoder remains independently bounded.
	std::filesystem::path ArtifactPath;
	for (const auto& Entry : std::filesystem::directory_iterator(Root.Path))
		if (Entry.path().extension() == ".pfartifact")
			ArtifactPath = Entry.path();
	PF_CHECK(Tests, !ArtifactPath.empty());
	if (!ArtifactPath.empty())
	{
		const auto Corrupt = Bytes("truncated artifact");
		PF_CHECK(Tests, WriteBytes(ArtifactPath, Corrupt));
		PF_CHECK(Tests, WarmStore.Read(Key).Status == DerivedDataArtifactReadStatus::Corrupt);
		PF_CHECK(Tests, !Detail::EnvironmentLightingArtifactCodec::DecodeEnvironment(Corrupt, Description));
		// The already-owned CPU product remains usable when the disposable entry is bad.
		PF_CHECK(Tests, Decoded.has_value());
		if (Decoded)
			PF_CHECK(Tests, SameEnvironment(Original, *Decoded));
	}

	DerivedDataArtifactStoreOptions DisabledOptions = Options;
	DisabledOptions.Enabled = false;
	DerivedDataArtifactStore DisabledStore(DisabledOptions);
	PF_CHECK(Tests, DisabledStore.Read(Key).Status == DerivedDataArtifactReadStatus::Unavailable);
	PF_CHECK(Tests, DisabledStore.Write(Key, *Encoded).Status == DerivedDataArtifactWriteStatus::Unavailable);
	const auto InMemoryFallback = Detail::EnvironmentLightingArtifactCodec::DecodeEnvironment(*Encoded, Description);
	PF_CHECK(Tests, InMemoryFallback.has_value());
	if (InMemoryFallback)
		PF_CHECK(Tests, SameEnvironment(Original, *InMemoryFallback));

	ImportedFloatImage Brdf;
	Brdf.Width = Description.BrdfLutSize;
	Brdf.Height = Description.BrdfLutSize;
	Brdf.RGBA32FPixels.assign(static_cast<size_t>(Brdf.Width) * Brdf.Height * 4, 0.5f);
	const auto BrdfEncoded = Detail::EnvironmentLightingArtifactCodec::EncodeBrdfLut(Brdf, Description.BrdfSamples);
	PF_CHECK(Tests, BrdfEncoded.has_value());
	if (BrdfEncoded)
	{
		const auto BrdfDecoded = Detail::EnvironmentLightingArtifactCodec::DecodeBrdfLut(
			*BrdfEncoded, Description.BrdfLutSize, Description.BrdfSamples);
		PF_CHECK(Tests, BrdfDecoded.has_value());
		if (BrdfDecoded)
			PF_CHECK(Tests, BrdfDecoded->RGBA32FPixels == Brdf.RGBA32FPixels);
	}

	// Exercise the same production CPU preparation path used by the cache: cold processing writes the shared disk
	// artifact, while a new service/session loads it without decoding or preprocessing the HDR source again.
	const std::filesystem::path ProjectRoot = Root.Path / "CpuPreparationProject";
	const std::filesystem::path EnvironmentDirectory = ProjectRoot / "Assets" / "Environments";
	std::error_code FileError;
	std::filesystem::create_directories(EnvironmentDirectory, FileError);
	PF_CHECK(Tests, !FileError);
	if (FileError)
		return;
	const std::filesystem::path HdrPath = EnvironmentDirectory / "tiny.hdr";
	const auto HdrBytes = MakeSmallRadianceHdr();
	PF_CHECK(Tests, WriteBytes(HdrPath, HdrBytes));
	const auto HdrMetadata = AssetMetadataSerializer::CreateForNewAsset(HdrPath);
	PF_CHECK(Tests, HdrMetadata.has_value());
	if (!HdrMetadata)
		return;
	AssetRegistry HdrRegistry;
	PF_CHECK(Tests, HdrRegistry.Rebuild(ProjectRoot).has_value());
	const DerivedDataArtifactStoreOptions CpuStoreOptions{ Root.Path / "CpuPreparationArtifacts", true,
		16 * 1024 * 1024, 8 * 1024 * 1024, 16 };
	const EnvironmentProcessingDesc CpuProcessing = MakeSmallDescription();
	Detail::EnvironmentCpuPreparationResult ColdProduct;
	{
		DerivedDataArtifactStore CpuStore(CpuStoreOptions);
		AssetPreparationService ColdService(MakeTestPreparationLimits());
		const auto Cold = RunCpuPreparation(ColdService, "cold-session", HdrMetadata->ID, ProjectRoot, HdrRegistry,
			CpuStore, CpuProcessing);
		PF_CHECK(Tests, Cold.has_value());
		if (!Cold)
			return;
		ColdProduct = *Cold;
		PF_CHECK(Tests, !ColdProduct.EnvironmentArtifactHit);
		PF_CHECK(Tests, !ColdProduct.BrdfArtifactHit);
		PF_CHECK(Tests, ColdProduct.RanEnvironmentPreprocessing);
		PF_CHECK(Tests, ColdProduct.RanBrdfPreprocessing);
		PF_CHECK(Tests, ColdProduct.Source == Detail::EnvironmentCpuProductSource::Processed);
		PF_CHECK(Tests, ColdProduct.Data.Environment.Size == CpuProcessing.EnvironmentFaceSize);
		if (ColdProduct.PreprocessEndTimestampNanoseconds > ColdProduct.PreprocessStartTimestampNanoseconds)
			std::cout << "G11 environment CPU cold: source observation+artifact lookup="
				<< (ColdProduct.DerivedCacheReadEndTimestampNanoseconds - ColdProduct.DerivedCacheReadStartTimestampNanoseconds) / 1000
				<< "us HDR decode="
				<< (ColdProduct.ImportEndTimestampNanoseconds - ColdProduct.ImportStartTimestampNanoseconds) / 1000
				<< "us process="
				<< (ColdProduct.PreprocessEndTimestampNanoseconds - ColdProduct.PreprocessStartTimestampNanoseconds) / 1000
				<< "us\n";
	}
	AssetRegistry WarmRegistry;
	PF_CHECK(Tests, WarmRegistry.Rebuild(ProjectRoot).has_value());
	{
		DerivedDataArtifactStore NewStoreInstance(CpuStoreOptions);
		AssetPreparationService WarmService(MakeTestPreparationLimits());
		const auto WarmProduct = RunCpuPreparation(WarmService, "warm-new-session", HdrMetadata->ID, ProjectRoot,
			WarmRegistry, NewStoreInstance, CpuProcessing);
		PF_CHECK(Tests, WarmProduct.has_value());
		if (WarmProduct)
		{
			PF_CHECK(Tests, WarmProduct->EnvironmentArtifactHit);
			PF_CHECK(Tests, WarmProduct->BrdfArtifactHit);
			PF_CHECK(Tests, !WarmProduct->RanEnvironmentPreprocessing);
			PF_CHECK(Tests, !WarmProduct->RanBrdfPreprocessing);
			PF_CHECK(Tests, WarmProduct->Source == Detail::EnvironmentCpuProductSource::DerivedArtifact);
			PF_CHECK(Tests, SameEnvironment(ColdProduct.Data, WarmProduct->Data));
			if (WarmProduct->DerivedCacheReadEndTimestampNanoseconds > WarmProduct->DerivedCacheReadStartTimestampNanoseconds)
				std::cout << "G11 environment CPU warm: source observation+artifact lookup="
					<< (WarmProduct->DerivedCacheReadEndTimestampNanoseconds - WarmProduct->DerivedCacheReadStartTimestampNanoseconds) / 1000
					<< "us process=0us\n";
		}
	}
}
