#include "Assets/Detail/AssetCpuProducts.h"

#include "Assets/AssetDependencyExtractor.h"
#include "Assets/AssetPathResolver.h"
#include "Assets/ImageAssetImporter.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <chrono>
#include <fstream>
#include <limits>
#include <span>

namespace PulseForge::Detail
{
	namespace
	{
		constexpr uint64_t MaximumImageSourceBytes = 128ull * 1024ull * 1024ull;

		uint64_t TimestampNanoseconds(std::chrono::steady_clock::time_point Timestamp)
		{
			return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
				Timestamp.time_since_epoch()).count());
		}

		AssetCpuPhaseTiming MakeTiming(
			std::chrono::steady_clock::time_point Start,
			std::chrono::steady_clock::time_point End)
		{
			return { TimestampNanoseconds(Start), TimestampNanoseconds(End) };
		}

		AssetImportRecipe MakeRecipe(const AssetImportObservation& O)
		{
			return { O.Asset, O.SubresourceKey, O.ImporterID, O.ImporterVersion, O.OutputSchemaVersion,
				O.EffectiveSettings, O.TargetID, O.Inputs, O.InputFingerprint };
		}

		bool SameRecipe(const AssetImportRecipe& A, const AssetImportRecipe& B)
		{
			return A.SourceAsset == B.SourceAsset && A.SubresourceKey == B.SubresourceKey && A.ImporterID == B.ImporterID &&
				A.ImporterVersion == B.ImporterVersion && A.OutputSchemaVersion == B.OutputSchemaVersion &&
				A.EffectiveSettings == B.EffectiveSettings && A.TargetID == B.TargetID && A.Inputs == B.Inputs &&
				A.InputFingerprint == B.InputFingerprint;
		}

		std::optional<DerivedDataArtifactKey> MeshKey(const AssetImportRecipe& R, const AssetID& Asset, std::string_view Settings)
		{
			if (R.SourceAsset != Asset || R.SubresourceKey != "mesh/0/primitive/0" ||
				R.ImporterID != "PulseForge.StaticGltfMeshImporter" || R.ImporterVersion != "1" || R.OutputSchemaVersion != 1 ||
				R.TargetID != "cpu-generic-v1" || R.EffectiveSettings != Settings)
				return std::nullopt;
			auto Fingerprint = ComputeAssetImportInputFingerprint(R);
			if (!Fingerprint || *Fingerprint != R.InputFingerprint) return std::nullopt;
			return DerivedDataArtifactKey{ "gltf-static-mesh", *Fingerprint, MeshProductSchemaVersion };
		}

		std::expected<std::vector<std::byte>, std::string> ReadImage(
			const std::filesystem::path& Path, std::stop_token Stop)
		{
			if (Stop.stop_requested()) return std::unexpected("Texture preparation cancelled before reading image bytes");
			std::error_code Ec;
			const auto Size = std::filesystem::file_size(Path, Ec);
			if (Ec || Size == 0 || Size > MaximumImageSourceBytes || Size > std::numeric_limits<size_t>::max() ||
				Size > static_cast<uintmax_t>(std::numeric_limits<std::streamsize>::max()))
				return std::unexpected("Image source is unavailable or exceeds its size limit");
			if (Stop.stop_requested()) return std::unexpected("Texture preparation cancelled before allocating image bytes");
			std::vector<std::byte> Bytes;
			Bytes.reserve(static_cast<size_t>(Size));
			std::ifstream In(Path, std::ios::binary);
			if (!In.is_open()) return std::unexpected("Image source could not be opened");
			std::array<std::byte, 256 * 1024> ReadBuffer{};
			for (uint64_t Offset = 0; Offset < Size; Offset += ReadBuffer.size())
			{
				if (Stop.stop_requested()) return std::unexpected("Texture preparation cancelled while reading image bytes");
				const size_t ChunkSize = static_cast<size_t>((std::min)(static_cast<uint64_t>(ReadBuffer.size()), Size - Offset));
				In.read(reinterpret_cast<char*>(ReadBuffer.data()), static_cast<std::streamsize>(ChunkSize));
				if (!In || In.bad() || static_cast<size_t>(In.gcount()) != ChunkSize)
					return std::unexpected("Image source could not be read completely");
				Bytes.insert(Bytes.end(), ReadBuffer.begin(), ReadBuffer.begin() + static_cast<std::ptrdiff_t>(ChunkSize));
			}
			if (Stop.stop_requested()) return std::unexpected("Texture preparation cancelled while reading image bytes");
			return Bytes;
		}

		std::expected<std::array<uint64_t, 2>, std::string> ImporterFingerprint(
			std::span<const std::byte> Bytes, std::stop_token Stop)
		{
			uint64_t A = 14695981039346656037ull, B = 7809847782465536322ull;
			constexpr size_t FingerprintChunkBytes = 1024 * 1024;
			for (size_t ChunkOffset = 0; ChunkOffset < Bytes.size(); ChunkOffset += FingerprintChunkBytes)
			{
				if (Stop.stop_requested())
					return std::unexpected("Texture preparation cancelled while fingerprinting image bytes");
				const size_t ChunkSize = (std::min)(FingerprintChunkBytes, Bytes.size() - ChunkOffset);
				for (size_t Offset = ChunkOffset; Offset < ChunkOffset + ChunkSize; ++Offset)
				{
					const uint8_t X = std::to_integer<uint8_t>(Bytes[Offset]);
					A = (A ^ X) * 1099511628211ull;
					B = (B ^ (X + 0x9du)) * 14029467366897019727ull;
					B ^= B >> 29;
				}
			}
			if (Stop.stop_requested())
				return std::unexpected("Texture preparation cancelled while fingerprinting image bytes");
			return std::array<uint64_t, 2>{ A, B };
		}

		DerivedDataArtifactKey TextureKey(const AssetContentFingerprint& Source, ImageColorSpace ColorSpace)
		{
			AssetFingerprintBuilder B("PulseForge.TextureMipProduct.v1");
			B.UpdateText("sourceAlgorithm", Source.Algorithm);
			B.UpdateText("sourceDigest", Source.DigestHex);
			B.UpdateText("decoder", "PulseForge.ImageRGBA8Importer.v1");
			B.UpdateText("mipAlgorithm", "box-filter-srgb-linear-v1");
			B.UpdateText("format", std::to_string(static_cast<uint8_t>(ColorSpace)));
			return { "image-rgba8-mips", B.Finish(), TextureProductSchemaVersion };
		}

		float SrgbToLinear(uint8_t V)
		{
			const float E = static_cast<float>(V) / 255.0f;
			return E <= 0.04045f ? E / 12.92f : std::pow((E + 0.055f) / 1.055f, 2.4f);
		}

		uint8_t LinearToSrgb(float V)
		{
			V = std::clamp(V, 0.0f, 1.0f);
			const float E = V <= 0.0031308f ? V * 12.92f : 1.055f * std::pow(V, 1.0f / 2.4f) - 0.055f;
			return static_cast<uint8_t>(std::lround(E * 255.0f));
		}

		std::expected<std::vector<PreparedMipLevel>, std::string> MakeMips(
			uint32_t W, uint32_t H, std::span<const uint8_t> Pixels, bool Srgb, std::stop_token Stop)
		{
			if (Stop.stop_requested()) return std::unexpected("Texture mip preparation was cancelled");
			std::vector<PreparedMipLevel> Levels;
			PreparedMipLevel Base{ W, H, {} };
			constexpr size_t CopyChunkBytes = 1024 * 1024;
			Base.Pixels.reserve(Pixels.size());
			for (size_t Offset = 0; Offset < Pixels.size(); Offset += CopyChunkBytes)
			{
				if (Stop.stop_requested()) return std::unexpected("Texture mip preparation was cancelled while copying the base level");
				const size_t ChunkSize = (std::min)(CopyChunkBytes, Pixels.size() - Offset);
				Base.Pixels.insert(Base.Pixels.end(), Pixels.begin() + static_cast<std::ptrdiff_t>(Offset),
					Pixels.begin() + static_cast<std::ptrdiff_t>(Offset + ChunkSize));
			}
			Levels.push_back(std::move(Base));
			while (W > 1 || H > 1)
			{
				if (Stop.stop_requested()) return std::unexpected("Texture mip preparation was cancelled");
				const auto& Previous = Levels.back();
				const uint32_t NW = (std::max)(1u, W / 2), NH = (std::max)(1u, H / 2);
				PreparedMipLevel Next{ NW, NH, {} };
				Next.Pixels.reserve(static_cast<size_t>(NW) * NH * 4);
				for (uint32_t Y = 0; Y < NH; ++Y)
				{
					if ((Y & 0x1fu) == 0 && Stop.stop_requested())
						return std::unexpected("Texture mip preparation was cancelled while filtering pixels");
					for (uint32_t X = 0; X < NW; ++X)
					{
						const uint32_t X0 = static_cast<uint32_t>((static_cast<uint64_t>(X) * W) / NW);
						const uint32_t X1 = static_cast<uint32_t>((static_cast<uint64_t>(X + 1) * W) / NW);
						const uint32_t Y0 = static_cast<uint32_t>((static_cast<uint64_t>(Y) * H) / NH);
						const uint32_t Y1 = static_cast<uint32_t>((static_cast<uint64_t>(Y + 1) * H) / NH);
						std::array<float, 4> Sum{}; uint32_t N = 0;
						for (uint32_t SY = Y0; SY < Y1; ++SY)
							for (uint32_t SX = X0; SX < X1; ++SX)
							{
								const size_t At = (static_cast<size_t>(SY) * W + SX) * 4;
								for (size_t C = 0; C < 4; ++C)
									Sum[C] += Srgb && C < 3 ? SrgbToLinear(Previous.Pixels[At + C]) : Previous.Pixels[At + C] / 255.0f;
								++N;
							}
						for (size_t C = 0; C < 4; ++C)
						{
							const float Avg = Sum[C] / static_cast<float>(N);
							Next.Pixels.push_back(Srgb && C < 3 ? LinearToSrgb(Avg) : static_cast<uint8_t>(std::lround(Avg * 255.0f)));
						}
					}
				}
				Levels.push_back(std::move(Next)); W = NW; H = NH;
			}
			return Levels;
		}

	}

	std::optional<DerivedDataArtifactKey> MakeMeshArtifactKey(
		const AssetImportRecipe& Recipe, const AssetID& Asset, std::string_view EffectiveSettings)
	{
		return MeshKey(Recipe, Asset, EffectiveSettings);
	}

	DerivedDataArtifactKey MakeTextureArtifactKey(const AssetContentFingerprint& Source, ImageColorSpace ColorSpace)
	{
		return TextureKey(Source, ColorSpace);
	}

	std::expected<std::vector<PreparedMipLevel>, std::string> BuildTextureMipChain(
		uint32_t Width, uint32_t Height, std::span<const uint8_t> SourcePixels, ImageColorSpace ColorSpace,
		std::stop_token CancellationToken)
	{
		return MakeMips(Width, Height, SourcePixels, ColorSpace == ImageColorSpace::Srgb, CancellationToken);
	}

	std::expected<AssetCpuProductResult<ImportedGltfMesh>, std::string> PrepareMeshCpuProduct(
		const AssetID& Asset, const std::filesystem::path& Root, const AssetRegistry& Registry, std::string_view Settings,
		DerivedDataArtifactStore* Store, std::stop_token Stop, MeshProcessor Processor)
	{
		const auto LookupStart = std::chrono::steady_clock::now();
		if (Stop.stop_requested()) return std::unexpected("Mesh preparation cancelled before inspection");
		std::optional<AssetImportRecipe> Observed;
		std::optional<DerivedDataArtifactKey> Key;
		std::optional<AssetContentFingerprint> RejectedProductChecksum;
		std::vector<AssetDependencyInputReceipt> ReadSet;
		// Observe inputs for every processing run, including when the disposable cache is
		// unavailable. Cache persistence is optional; publishing a product prepared from
		// inputs that changed during processing is not.
		auto Inspection = AssetDependencyExtractor::ExtractAsset(Root, Asset, Registry, Stop);
		if (!Inspection)
		{
			if (Stop.stop_requested()) return std::unexpected("Mesh preparation cancelled during source inspection");
			return std::unexpected("Mesh source could not be inspected for a stable processing read set: " + Inspection.error().Message);
		}
		auto O = std::find_if(Inspection->ImportObservations.begin(), Inspection->ImportObservations.end(),
			[](const auto& X) { return X.SubresourceKey == "mesh/0/primitive/0"; });
		auto S = std::find_if(Inspection->Subresources.begin(), Inspection->Subresources.end(),
			[](const auto& X) { return X.Identity.SubresourceKey == "mesh/0/primitive/0"; });
		if (O == Inspection->ImportObservations.end() || S == Inspection->Subresources.end() ||
			(S->Classification != AssetSubresourceClassification::SupportedCandidate &&
			 S->Classification != AssetSubresourceClassification::Imported) || O->EffectiveSettings != Settings)
			return std::unexpected("Mesh source does not contain the requested supported output and settings");
		Observed = MakeRecipe(*O);
		Key = MakeMeshArtifactKey(*Observed, Asset, Settings);
		ReadSet = std::move(Inspection->ReadSet);
		if (Store)
		{
			if (Key)
			{
				auto Cached = Store->Read(*Key);
				if (Cached.Status == DerivedDataArtifactReadStatus::Hit)
				{
					auto Product = DecodeMeshProduct(Cached.Payload);
					auto InputsCurrent = AssetDependencyExtractor::ValidateInputReceipts(Root, ReadSet, Stop);
					if (Stop.stop_requested()) return std::unexpected("Mesh preparation cancelled during cache validation");
					if (Product && InputsCurrent)
					{
						const auto LookupEnd = std::chrono::steady_clock::now();
						AssetCpuProductResult<ImportedGltfMesh> Result{ std::move(*Product), AssetCpuProductCacheOutcome::ArtifactHit,
							static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(LookupEnd - LookupStart).count()), 0, 0 };
						Result.ObservationAndLookup = MakeTiming(LookupStart, LookupEnd);
						return Result;
					}
					if (!Product)
						RejectedProductChecksum = Cached.EntryChecksum;
				}
			}
		}
		if (Stop.stop_requested()) return std::unexpected("Mesh preparation cancelled before import");
		if (!Processor)
			Processor = [Stop](const AssetID& ID, const std::filesystem::path& Project, const AssetRegistry& Assets)
				-> std::expected<GltfMeshImportProduct, std::string>
			{
				auto Product = GltfMeshImporter::ProcessStaticPrimitive(ID, Project, Assets, 0, 0, Stop);
				if (!Product) return std::unexpected(Product.error().Message);
				return std::move(*Product);
			};
		const auto LookupEnd = std::chrono::steady_clock::now();
		const uint64_t LookupMicroseconds = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(LookupEnd - LookupStart).count());
		const auto ProcessStart = std::chrono::steady_clock::now();
		auto Product = Processor(Asset, Root, Registry);
		const auto ProcessEnd = std::chrono::steady_clock::now();
		const uint64_t ProcessMicroseconds = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(ProcessEnd - ProcessStart).count());
		if (Stop.stop_requested()) return std::unexpected("Mesh preparation cancelled during import");
		if (!Product) return std::unexpected(Product.error());
		if (Product->Recipe.EffectiveSettings != Settings) return std::unexpected("Mesh import settings changed during preparation");
		if (Stop.stop_requested()) return std::unexpected("Mesh preparation cancelled after import");
		if (!Observed || !SameRecipe(Product->Recipe, *Observed))
			return std::unexpected("Mesh inputs or processing recipe changed during preparation");
		auto InputsCurrent = AssetDependencyExtractor::ValidateInputReceipts(Root, ReadSet, Stop);
		if (Stop.stop_requested()) return std::unexpected("Mesh preparation cancelled during final input validation");
		if (!InputsCurrent)
			return std::unexpected("Mesh inputs changed during preparation; the CPU product was rejected as stale");
		AssetCpuProductCacheOutcome Outcome = AssetCpuProductCacheOutcome::ProcessedStoreUnavailable;
		uint64_t WriteMicroseconds = 0;
		auto WriteStart = std::chrono::steady_clock::time_point{};
		auto WriteEnd = WriteStart;
		if (Store && Key)
		{
			if (Stop.stop_requested()) return std::unexpected("Mesh preparation cancelled before artifact encoding");
			WriteStart = std::chrono::steady_clock::now();
			if (auto Bytes = EncodeMeshProduct(Product->Mesh, Stop); Bytes)
			{
				if (Stop.stop_requested()) return std::unexpected("Mesh preparation cancelled before artifact persistence");
				const auto Write = RejectedProductChecksum
					? Store->ReplaceRejectedProduct(*Key, *RejectedProductChecksum, *Bytes)
					: Store->Write(*Key, *Bytes);
				if (Stop.stop_requested()) return std::unexpected("Mesh preparation cancelled after artifact persistence");
				if (Write.Status == DerivedDataArtifactWriteStatus::Written || Write.Status == DerivedDataArtifactWriteStatus::ExistingValid)
					Outcome = AssetCpuProductCacheOutcome::ProcessedAndStored;
			}
			else if (Stop.stop_requested()) return std::unexpected("Mesh preparation cancelled during artifact encoding");
			WriteEnd = std::chrono::steady_clock::now();
			WriteMicroseconds = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(WriteEnd - WriteStart).count());
		}
		if (Stop.stop_requested()) return std::unexpected("Mesh preparation cancelled before publishing its CPU product");
		AssetCpuProductResult<ImportedGltfMesh> Result{ std::move(Product->Mesh), Outcome,
			LookupMicroseconds, ProcessMicroseconds, WriteMicroseconds };
		Result.ObservationAndLookup = MakeTiming(LookupStart, LookupEnd);
		Result.Processing = MakeTiming(ProcessStart, ProcessEnd);
		if (WriteEnd > WriteStart) Result.CacheWrite = MakeTiming(WriteStart, WriteEnd);
		return Result;
	}

	std::expected<AssetCpuProductResult<PreparedTextureCpuProduct>, std::string> PrepareTextureCpuProduct(
		const AssetID& Asset, const std::filesystem::path& Root, const AssetRegistry& Registry, ImageColorSpace ColorSpace,
		DerivedDataArtifactStore* Store, std::stop_token Stop, ImageProcessor Processor)
	{
		const auto LookupStart = std::chrono::steady_clock::now();
		if (ColorSpace != ImageColorSpace::Linear && ColorSpace != ImageColorSpace::Srgb) return std::unexpected("Unsupported image color space");
		if (Stop.stop_requested()) return std::unexpected("Texture preparation cancelled before source read");
		auto Record = Registry.Find(Asset);
		if (!Record) return std::unexpected("Image asset is absent from captured registry");
		auto Resolved = AssetPathResolver::ResolveManagedSourcePath(Root, Record->ProjectRelativePath);
		if (!Resolved) return std::unexpected(Resolved.error().Message);
		auto Source = ReadImage(*Resolved, Stop);
		if (!Source)
		{
			if (Stop.stop_requested()) return std::unexpected("Texture preparation cancelled while reading source bytes");
			return std::unexpected(Source.error());
		}
		auto FingerprintResult = ComputeAssetContentFingerprintCancellable(*Source, Stop);
		if (!FingerprintResult) return std::unexpected("Texture preparation cancelled while computing the source fingerprint");
		const AssetContentFingerprint Fingerprint = std::move(*FingerprintResult);
		auto RawFingerprintResult = ImporterFingerprint(*Source, Stop);
		if (!RawFingerprintResult) return std::unexpected(RawFingerprintResult.error());
		const auto RawFingerprint = *RawFingerprintResult;
		const DerivedDataArtifactKey Key = MakeTextureArtifactKey(Fingerprint, ColorSpace);
		std::optional<AssetContentFingerprint> RejectedProductChecksum;
		std::vector<std::byte>().swap(*Source);
		if (Store)
		{
			auto Cached = Store->Read(Key);
			if (Cached.Status == DerivedDataArtifactReadStatus::Hit)
			{
				auto Product = DecodeTextureProduct(Cached.Payload);
				if (Product && Product->ColorSpace != ColorSpace)
					Product = std::unexpected(std::string("Texture artifact color space does not match the requested interpretation"));
				if (Product)
				{
					auto Current = ReadImage(*Resolved, Stop);
					if (Stop.stop_requested()) return std::unexpected("Texture preparation cancelled during cache validation");
					if (!Current) return std::unexpected("Image source could not be reread during cache validation");
					auto CurrentFingerprint = ComputeAssetContentFingerprintCancellable(*Current, Stop);
					if (!CurrentFingerprint || *CurrentFingerprint != Fingerprint)
						return std::unexpected(Stop.stop_requested()
							? "Texture preparation cancelled during cache validation"
							: "Image source changed during cache lookup");
					const auto LookupEnd = std::chrono::steady_clock::now();
					AssetCpuProductResult<PreparedTextureCpuProduct> Result{ std::move(*Product), AssetCpuProductCacheOutcome::ArtifactHit,
						static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(LookupEnd - LookupStart).count()), 0, 0 };
					Result.ObservationAndLookup = MakeTiming(LookupStart, LookupEnd);
					return Result;
				}
				RejectedProductChecksum = Cached.EntryChecksum;
			}
		}
		if (Stop.stop_requested()) return std::unexpected("Texture preparation cancelled before decode");
		if (!Processor)
			Processor = [](const AssetID& ID, const std::filesystem::path& Project, const AssetRegistry& Assets)
				-> std::expected<ImportedImage, std::string>
			{
				auto Image = ImageAssetImporter::ImportRGBA8(ID, Project, Assets);
				if (!Image) return std::unexpected(Image.error().Message);
				return std::move(*Image);
			};
		const auto LookupEnd = std::chrono::steady_clock::now();
		const uint64_t LookupMicroseconds = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(LookupEnd - LookupStart).count());
		const auto ProcessStart = std::chrono::steady_clock::now();
		auto Image = Processor(Asset, Root, Registry);
		if (Stop.stop_requested()) return std::unexpected("Texture preparation cancelled after image decode");
		if (!Image) return std::unexpected(Image.error());
		if (Image->SourceFingerprint != RawFingerprint) return std::unexpected("Image changed between key observation and decoding");
		PreparedTextureCpuProduct Product;
		Product.Width = Image->Width; Product.Height = Image->Height; Product.ColorSpace = ColorSpace;
		auto Mips = MakeMips(Image->Width, Image->Height, Image->RGBA8Pixels, ColorSpace == ImageColorSpace::Srgb, Stop);
		if (!Mips)
			return std::unexpected(Stop.stop_requested()
				? "Texture preparation cancelled during mip processing"
				: Mips.error());
		Product.MipLevels = std::move(*Mips);
		const auto ProcessEnd = std::chrono::steady_clock::now();
		const uint64_t ProcessMicroseconds = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(ProcessEnd - ProcessStart).count());
		if (Stop.stop_requested()) return std::unexpected("Texture preparation cancelled after mip processing");
		auto Current = ReadImage(*Resolved, Stop);
		if (Stop.stop_requested()) return std::unexpected("Texture preparation cancelled during final source validation");
		if (!Current) return std::unexpected("Image source could not be reread after CPU processing");
		auto CurrentFingerprint = ComputeAssetContentFingerprintCancellable(*Current, Stop);
		if (!CurrentFingerprint || *CurrentFingerprint != Fingerprint)
			return std::unexpected(Stop.stop_requested()
				? "Texture preparation cancelled during final source validation"
				: "Image source changed during CPU processing");
		AssetCpuProductCacheOutcome Outcome = AssetCpuProductCacheOutcome::ProcessedStoreUnavailable;
		uint64_t WriteMicroseconds = 0;
		auto WriteStart = std::chrono::steady_clock::time_point{};
		auto WriteEnd = WriteStart;
		if (Store)
		{
			if (Stop.stop_requested()) return std::unexpected("Texture preparation cancelled before artifact encoding");
			WriteStart = std::chrono::steady_clock::now();
			if (auto Bytes = EncodeTextureProduct(Product, Stop); Bytes)
			{
				if (Stop.stop_requested()) return std::unexpected("Texture preparation cancelled before artifact persistence");
				const auto Write = RejectedProductChecksum
					? Store->ReplaceRejectedProduct(Key, *RejectedProductChecksum, *Bytes)
					: Store->Write(Key, *Bytes);
				if (Stop.stop_requested()) return std::unexpected("Texture preparation cancelled after artifact persistence");
				if (Write.Status == DerivedDataArtifactWriteStatus::Written || Write.Status == DerivedDataArtifactWriteStatus::ExistingValid)
					Outcome = AssetCpuProductCacheOutcome::ProcessedAndStored;
			}
			else if (Stop.stop_requested()) return std::unexpected("Texture preparation cancelled during artifact encoding");
			WriteEnd = std::chrono::steady_clock::now();
			WriteMicroseconds = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(WriteEnd - WriteStart).count());
		}
		if (Stop.stop_requested()) return std::unexpected("Texture preparation cancelled before publishing its CPU product");
		AssetCpuProductResult<PreparedTextureCpuProduct> Result{ std::move(Product), Outcome,
			LookupMicroseconds, ProcessMicroseconds, WriteMicroseconds };
		Result.ObservationAndLookup = MakeTiming(LookupStart, LookupEnd);
		Result.Processing = MakeTiming(ProcessStart, ProcessEnd);
		if (WriteEnd > WriteStart) Result.CacheWrite = MakeTiming(WriteStart, WriteEnd);
		return Result;
	}
} // namespace PulseForge::Detail
