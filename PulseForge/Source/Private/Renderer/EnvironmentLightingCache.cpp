#include "Renderer/EnvironmentLightingCache.h"

#include "Assets/AssetPathResolver.h"
#include "Assets/AssetTypes.h"
#include "Assets/ImageAssetImporter.h"
#include "Core/Diagnostics.h"
#include "Core/ScopedProfileTimer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <new>
#include <span>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>

namespace PulseForge
{
	namespace
	{
		constexpr uint32_t EnvironmentCacheVersion = 1;
		constexpr uint32_t EnvironmentAlgorithmVersion = 1;
		constexpr uint32_t BrdfCacheVersion = 1;
		constexpr uint32_t EnvironmentProductSchemaVersion = 1;
		constexpr uint32_t EnvironmentCodecVersion = 1;
		constexpr uint32_t BrdfCodecVersion = 1;
		constexpr uint64_t MaximumCacheBytes = 64ull * 1024ull * 1024ull;
		constexpr uint64_t MaximumSourceBytes = 128ull * 1024ull * 1024ull;
		constexpr size_t SourceFingerprintChunkBytes = 64 * 1024;

		[[nodiscard]] uint64_t GetMonotonicTimestampNanoseconds() noexcept
		{
			return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count());
		}

#pragma pack(push, 1)
		struct EnvironmentCacheHeader
		{
			char Magic[8] = { 'P', 'F', 'I', 'B', 'L', 'D', 'D', '1' };
			uint32_t Version = EnvironmentCacheVersion;
			uint64_t FingerprintA = 0;
			uint64_t FingerprintB = 0;
			uint32_t AlgorithmVersion = EnvironmentAlgorithmVersion;
			uint32_t EnvironmentFaceSize = 0;
			uint32_t IrradianceFaceSize = 0;
			uint32_t SpecularFaceSize = 0;
			uint32_t BrdfLutSize = 0;
			uint32_t IrradianceSamples = 0;
			uint32_t PrefilterSamples = 0;
			uint32_t BrdfSamples = 0;
			uint32_t SpecularMipCount = 0;
		};

		struct BrdfCacheHeader
		{
			char Magic[8] = { 'P', 'F', 'B', 'R', 'D', 'F', '0', '1' };
			uint32_t Version = BrdfCacheVersion;
			uint32_t AlgorithmVersion = EnvironmentAlgorithmVersion;
			uint32_t Size = 0;
			uint32_t Samples = 0;
		};
#pragma pack(pop)

		EnvironmentLightingCacheError MakeError(const AssetID& Asset, std::string Message)
		{
			return { Asset, std::move(Message) };
		}

		EnvironmentProcessingDesc GetProcessingDescription()
		{
			EnvironmentProcessingDesc Description;
			Description.EnvironmentFaceSize = 256;
			Description.IrradianceFaceSize = 32;
			Description.SpecularFaceSize = 128;
			Description.BrdfLutSize = 256;
			return Description;
		}

		uint32_t GetMipCount(uint32_t Size)
		{
			uint32_t Count = 1;
			while (Size > 1)
			{
				Size >>= 1;
				++Count;
			}
			return Count;
		}

		std::filesystem::path GetCacheDirectory(const std::filesystem::path& ProjectRoot)
		{
			return ProjectRoot / ".pulseforge" / "derived" / "environment-lighting";
		}

		std::filesystem::path GetArtifactCacheDirectory(const std::filesystem::path& ProjectRoot)
		{
			return ProjectRoot / ".pulseforge" / "DerivedData";
		}

		std::string MakeCompatibilityPreparationSession(const std::filesystem::path& ProjectRoot)
		{
			static std::atomic<uint64_t> NextSession{ 1 };
			return ProjectRoot.lexically_normal().generic_string() + "#environment-session-" +
				std::to_string(NextSession.fetch_add(1, std::memory_order_relaxed));
		}

		AssetPreparationServiceLimits MakeCompatibilityPreparationLimits()
		{
			AssetPreparationServiceLimits Limits;
			Limits.MaximumWorkers = 1;
			Limits.MaximumOutstandingJobs = 8;
			Limits.MaximumSubscribers = 32;
			Limits.MaximumReservedWorkingBytes = 512ull * 1024ull * 1024ull;
			Limits.MaximumCompletedPayloadBytes = 256ull * 1024ull * 1024ull;
			return Limits;
		}

		std::filesystem::path GetEnvironmentCachePath(
			const std::filesystem::path& ProjectRoot,
			const AssetID& Asset,
			const std::array<uint64_t, 2>& Fingerprint,
			const EnvironmentProcessingDesc& Description)
		{
			std::ostringstream Name;
			Name << Asset.ToString() << '-' << std::hex << std::setw(16) << std::setfill('0') << Fingerprint[0]
				<< std::setw(16) << Fingerprint[1] << std::dec << "-a" << EnvironmentAlgorithmVersion
				<< "-e" << Description.EnvironmentFaceSize << "-i" << Description.IrradianceFaceSize
				<< "-s" << Description.SpecularFaceSize << "-b" << Description.BrdfLutSize
				<< "-n" << Description.IrradianceSamples << '-' << Description.PrefilterSamples << '-' << Description.BrdfSamples
				<< ".pfibl";
			return GetCacheDirectory(ProjectRoot) / Name.str();
		}

		std::filesystem::path GetBrdfCachePath(
			const std::filesystem::path& ProjectRoot,
			uint32_t Size,
			uint32_t Samples)
		{
			return GetCacheDirectory(ProjectRoot) / ("brdf-a" + std::to_string(EnvironmentAlgorithmVersion) + "-" +
				std::to_string(Size) + "-" + std::to_string(Samples) + ".pfbrdf");
		}

		bool IsFiniteImage(const ImportedFloatImage& Image)
		{
			return std::ranges::all_of(Image.RGBA32FPixels, [](float Value) { return std::isfinite(Value); });
		}

		std::optional<ImportedFloatImage> ReadBrdfCache(
			const std::filesystem::path& Path,
			uint32_t Size,
			uint32_t Samples)
		{
			std::error_code Error;
			const uintmax_t FileSize = std::filesystem::file_size(Path, Error);
			const uint64_t ExpectedPixels = static_cast<uint64_t>(Size) * Size * 4;
			if (Error || FileSize != sizeof(BrdfCacheHeader) + ExpectedPixels * sizeof(float) || FileSize > MaximumCacheBytes)
				return std::nullopt;
			std::ifstream Input(Path, std::ios::binary);
			BrdfCacheHeader Header;
			if (!Input.read(reinterpret_cast<char*>(&Header), sizeof(Header)) ||
				std::memcmp(Header.Magic, BrdfCacheHeader{}.Magic, sizeof(Header.Magic)) != 0 ||
				Header.Version != BrdfCacheVersion || Header.AlgorithmVersion != EnvironmentAlgorithmVersion ||
				Header.Size != Size || Header.Samples != Samples)
				return std::nullopt;
			ImportedFloatImage Image;
			Image.Width = Size;
			Image.Height = Size;
			try
			{
				Image.RGBA32FPixels.resize(static_cast<size_t>(ExpectedPixels));
			}
			catch (const std::bad_alloc&)
			{
				return std::nullopt;
			}
			if (!Input.read(reinterpret_cast<char*>(Image.RGBA32FPixels.data()),
				static_cast<std::streamsize>(Image.RGBA32FPixels.size() * sizeof(float))) || !IsFiniteImage(Image))
				return std::nullopt;
			return Image;
		}

		uint64_t GetEnvironmentFloatCount(const EnvironmentProcessingDesc& Description)
		{
			uint64_t Count = static_cast<uint64_t>(Description.EnvironmentFaceSize) * Description.EnvironmentFaceSize * 6 * 4;
			Count += static_cast<uint64_t>(Description.IrradianceFaceSize) * Description.IrradianceFaceSize * 6 * 4;
			for (uint32_t Size = Description.SpecularFaceSize;; Size = std::max(1u, Size / 2))
			{
				Count += static_cast<uint64_t>(Size) * Size * 6 * 4;
				if (Size == 1)
					break;
			}
			return Count;
		}

		bool ReadLevel(std::ifstream& Input, FloatCubeLevel& Level, uint32_t Size)
		{
			Level.Size = Size;
			const size_t FaceFloatCount = static_cast<size_t>(Size) * Size * 4;
			try
			{
				for (std::vector<float>& Face : Level.Faces)
					Face.resize(FaceFloatCount);
			}
			catch (const std::bad_alloc&)
			{
				return false;
			}
			for (std::vector<float>& Face : Level.Faces)
			{
				if (!Input.read(reinterpret_cast<char*>(Face.data()),
					static_cast<std::streamsize>(Face.size() * sizeof(float))))
					return false;
				if (!std::ranges::all_of(Face, [](float Value) { return std::isfinite(Value); }))
					return false;
			}
			return true;
		}

		std::optional<EnvironmentLightingData> ReadEnvironmentCache(
			const std::filesystem::path& Path,
			const AssetID& Asset,
			const std::array<uint64_t, 2>& Fingerprint,
			const EnvironmentProcessingDesc& Description)
		{
			std::error_code Error;
			const uintmax_t FileSize = std::filesystem::file_size(Path, Error);
			const uint64_t PixelBytes = GetEnvironmentFloatCount(Description) * sizeof(float);
			if (Error || FileSize != sizeof(EnvironmentCacheHeader) + PixelBytes || FileSize > MaximumCacheBytes)
				return std::nullopt;
			std::ifstream Input(Path, std::ios::binary);
			EnvironmentCacheHeader Header;
			if (!Input.read(reinterpret_cast<char*>(&Header), sizeof(Header)) ||
				std::memcmp(Header.Magic, EnvironmentCacheHeader{}.Magic, sizeof(Header.Magic)) != 0 ||
				Header.Version != EnvironmentCacheVersion || Header.FingerprintA != Fingerprint[0] ||
				Header.FingerprintB != Fingerprint[1] || Header.AlgorithmVersion != EnvironmentAlgorithmVersion ||
				Header.EnvironmentFaceSize != Description.EnvironmentFaceSize ||
				Header.IrradianceFaceSize != Description.IrradianceFaceSize ||
				Header.SpecularFaceSize != Description.SpecularFaceSize || Header.BrdfLutSize != Description.BrdfLutSize ||
				Header.IrradianceSamples != Description.IrradianceSamples ||
				Header.PrefilterSamples != Description.PrefilterSamples || Header.BrdfSamples != Description.BrdfSamples ||
				Header.SpecularMipCount != GetMipCount(Description.SpecularFaceSize))
				return std::nullopt;

			EnvironmentLightingData Data;
			if (!ReadLevel(Input, Data.Environment, Description.EnvironmentFaceSize) ||
				!ReadLevel(Input, Data.DiffuseIrradiance, Description.IrradianceFaceSize))
				return std::nullopt;
			try
			{
				Data.PrefilteredSpecular.reserve(Header.SpecularMipCount);
				uint32_t MipSize = Description.SpecularFaceSize;
				for (uint32_t Mip = 0; Mip < Header.SpecularMipCount; ++Mip)
				{
					FloatCubeLevel& Level = Data.PrefilteredSpecular.emplace_back();
					if (!ReadLevel(Input, Level, MipSize))
						return std::nullopt;
					MipSize = std::max(1u, MipSize / 2);
				}
			}
			catch (const std::bad_alloc&)
			{
				return std::nullopt;
			}
			(void)Asset;
			return Data;
		}

		std::expected<TextureHandle, TextureError> CreateCubeTexture(RenderDevice& Runtime, const FloatCubeLevel& Level, uint32_t MipLevels,
			std::span<const FloatCubeLevel> Mips, std::string DebugName)
		{
			TextureDesc Description;
			Description.Width = Mips.empty() ? Level.Size : Mips.front().Size;
			Description.Height = Description.Width;
			Description.Format = TextureFormat::RGBA32_Float;
			Description.Dimension = TextureDimension::TextureCube;
			Description.MipLevels = MipLevels;
			Description.DebugName = std::move(DebugName);

			std::vector<TextureSubresourceData> Subresources;
			Subresources.reserve(static_cast<size_t>(MipLevels) * 6);
			for (uint32_t Mip = 0; Mip < MipLevels; ++Mip)
			{
				const FloatCubeLevel& Source = Mips.empty() ? Level : Mips[Mip];
				for (uint32_t Face = 0; Face < 6; ++Face)
				{
					const std::span<const float> Pixels(Source.Faces[Face]);
					Subresources.push_back({ Mip, Face, std::as_bytes(Pixels), static_cast<size_t>(Source.Size) * 4 * sizeof(float) });
				}
			}
			return Runtime.CreateTexture(Description, Subresources);
		}

		EnvironmentLightingCacheError ToCacheError(const AssetID& Asset, std::string Message)
		{
			return { Asset, std::move(Message) };
		}

		void AppendU32(std::vector<std::byte>& Bytes, uint32_t Value)
		{
			for (unsigned Shift = 0; Shift < 32; Shift += 8)
				Bytes.push_back(static_cast<std::byte>((Value >> Shift) & 0xff));
		}

		bool ReadU32(std::span<const std::byte> Bytes, size_t& Offset, uint32_t& Value)
		{
			if (Offset > Bytes.size() || Bytes.size() - Offset < 4)
				return false;
			Value = 0;
			for (unsigned Shift = 0; Shift < 32; Shift += 8)
				Value |= static_cast<uint32_t>(std::to_integer<uint8_t>(Bytes[Offset++])) << Shift;
			return true;
		}

		void AppendFloat(std::vector<std::byte>& Bytes, float Value)
		{
			const uint32_t Bits = std::bit_cast<uint32_t>(Value);
			AppendU32(Bytes, Bits);
		}

		bool ReadFloat(std::span<const std::byte> Bytes, size_t& Offset, float& Value)
		{
			uint32_t Bits = 0;
			if (!ReadU32(Bytes, Offset, Bits))
				return false;
			Value = std::bit_cast<float>(Bits);
			return std::isfinite(Value);
		}

		bool IsValidEnvironmentDescription(const EnvironmentProcessingDesc& Description)
		{
			return Description.EnvironmentFaceSize > 0 && Description.EnvironmentFaceSize <= 2048 &&
				Description.IrradianceFaceSize > 0 && Description.IrradianceFaceSize <= 2048 &&
				Description.SpecularFaceSize > 0 && Description.SpecularFaceSize <= 2048 &&
				Description.BrdfLutSize > 0 && Description.BrdfLutSize <= 2048 &&
				Description.IrradianceSamples > 0 && Description.IrradianceSamples <= 4096 &&
				Description.PrefilterSamples > 0 && Description.PrefilterSamples <= 4096 &&
				Description.BrdfSamples > 0 && Description.BrdfSamples <= 4096 &&
				GetEnvironmentFloatCount(Description) <= MaximumCacheBytes / sizeof(float);
		}

		bool AppendLevelFloats(std::vector<std::byte>& Payload, const FloatCubeLevel& Level, uint32_t ExpectedSize)
		{
			const uint64_t FloatCount = static_cast<uint64_t>(ExpectedSize) * ExpectedSize * 4;
			if (Level.Size != ExpectedSize)
				return false;
			for (const std::vector<float>& Face : Level.Faces)
			{
				if (Face.size() != FloatCount || !std::ranges::all_of(Face, [](float Value) { return std::isfinite(Value); }))
					return false;
				for (float Value : Face)
					AppendFloat(Payload, Value);
			}
			return true;
		}

		bool ReadLevelFloats(
			std::span<const std::byte> Payload,
			size_t& Offset,
			FloatCubeLevel& Level,
			uint32_t Size)
		{
			Level.Size = Size;
			const uint64_t FloatCount = static_cast<uint64_t>(Size) * Size * 4;
			try
			{
				for (std::vector<float>& Face : Level.Faces)
					Face.resize(static_cast<size_t>(FloatCount));
			}
			catch (const std::bad_alloc&)
			{
				return false;
			}
			for (std::vector<float>& Face : Level.Faces)
				for (float& Value : Face)
					if (!ReadFloat(Payload, Offset, Value))
						return false;
			return true;
		}

		struct EnvironmentSourceObservation
		{
			AssetContentFingerprint Content;
			std::array<uint64_t, 2> LegacyFingerprint{};
		};

		size_t GetPreparedEnvironmentRetainedBytes(const EnvironmentLightingData& Data) noexcept
		{
			constexpr size_t Maximum = (std::numeric_limits<size_t>::max)();
			size_t Total = 0;
			auto Add = [&Total](size_t Count)
			{
				constexpr size_t Limit = (std::numeric_limits<size_t>::max)();
				if (Count > Limit - Total)
					Total = Limit;
				else
					Total += Count;
			};
			auto AddLevel = [&Add](const FloatCubeLevel& Level)
			{
				constexpr size_t Limit = (std::numeric_limits<size_t>::max)();
				for (const auto& Face : Level.Faces)
					Add(Face.size() > (Limit / sizeof(float)) ? Limit : Face.size() * sizeof(float));
			};
			AddLevel(Data.Environment);
			AddLevel(Data.DiffuseIrradiance);
			for (const FloatCubeLevel& Level : Data.PrefilteredSpecular)
				AddLevel(Level);
			Add(Data.BrdfIntegrationLut.RGBA32FPixels.size() > (Maximum / sizeof(float))
				? Maximum : Data.BrdfIntegrationLut.RGBA32FPixels.size() * sizeof(float));
			return Total;
		}

		std::expected<EnvironmentSourceObservation, std::string> ObserveEnvironmentSource(
			const AssetID& Asset,
			const std::filesystem::path& ProjectRoot,
			const AssetRegistry& Registry)
		{
			const auto Record = Registry.Find(Asset);
			if (!Record)
				return std::unexpected("Environment asset is not present in the project registry");
			if (!HasAssetTypeExtension(Record->ProjectRelativePath, AssetFileType::Environment))
				return std::unexpected("Environment cache only accepts managed HDR assets");
			auto Resolved = AssetPathResolver::ResolveManagedSourcePath(ProjectRoot, Record->ProjectRelativePath);
			if (!Resolved)
				return std::unexpected(Resolved.error().Message);

			std::error_code FileError;
			const uintmax_t FileSize = std::filesystem::file_size(*Resolved, FileError);
			if (FileError || FileSize == 0 || FileSize > MaximumSourceBytes ||
				FileSize > static_cast<uintmax_t>(std::numeric_limits<int>::max()) ||
				FileSize > static_cast<uintmax_t>(std::numeric_limits<std::streamsize>::max()))
				return std::unexpected("Environment source is inaccessible, empty, or exceeds the bounded import size");

			AssetFingerprintBuilder Builder("PulseForge.AssetInputBytes.v1");
			const std::string ByteLength = std::to_string(FileSize);
			Builder.UpdateText("byteLength", ByteLength);
			EnvironmentSourceObservation Observation;
			Observation.LegacyFingerprint = { 14695981039346656037ull, 7809847782465536322ull };
			std::array<std::byte, SourceFingerprintChunkBytes> Buffer{};
			uint64_t Remaining = FileSize;
			std::ifstream Input(*Resolved, std::ios::binary);
			if (!Input)
				return std::unexpected("Could not open environment source for bounded fingerprinting");
			while (Remaining > 0)
			{
				const size_t ChunkSize = static_cast<size_t>(std::min<uint64_t>(Remaining, Buffer.size()));
				if (!Input.read(reinterpret_cast<char*>(Buffer.data()), static_cast<std::streamsize>(ChunkSize)))
					return std::unexpected("Environment source changed or failed during fingerprinting");
				const std::span<const std::byte> Chunk(Buffer.data(), ChunkSize);
				Builder.UpdateBytes("chunk", Chunk);
				for (const std::byte Byte : Chunk)
				{
					const uint8_t Value = std::to_integer<uint8_t>(Byte);
					Observation.LegacyFingerprint[0] = (Observation.LegacyFingerprint[0] ^ Value) * 1099511628211ull;
					Observation.LegacyFingerprint[1] = (Observation.LegacyFingerprint[1] ^ (Value + 0x9du)) * 14029467366897019727ull;
					Observation.LegacyFingerprint[1] ^= Observation.LegacyFingerprint[1] >> 29;
				}
				Remaining -= ChunkSize;
			}
			if (Input.peek() != std::char_traits<char>::eof())
				return std::unexpected("Environment source grew while it was being fingerprinted");
			Observation.Content = Builder.Finish();
			return Observation;
		}

		DerivedDataArtifactStoreOptions ResolveArtifactStoreOptions(
			const std::filesystem::path& ProjectRoot,
			DerivedDataArtifactStoreOptions Options)
		{
			if (Options.Root.empty() && !ProjectRoot.empty())
				Options.Root = GetArtifactCacheDirectory(ProjectRoot);
			return Options;
		}
	}

	DerivedDataArtifactKey Detail::EnvironmentLightingArtifactCodec::MakeEnvironmentKey(
		const AssetID& Asset,
		const AssetContentFingerprint& SourceContent,
		const EnvironmentProcessingDesc& Description)
	{
		AssetFingerprintBuilder Builder("PulseForge.EnvironmentLightingRecipe.v1");
		Builder.UpdateText("asset", Asset.ToString());
		Builder.UpdateText("source-algorithm", SourceContent.Algorithm);
		Builder.UpdateText("source-digest", SourceContent.DigestHex);
		Builder.UpdateText("importer", "PulseForge.RadianceHDR.v1");
		Builder.UpdateText("environment-algorithm", std::to_string(EnvironmentAlgorithmVersion));
		Builder.UpdateText("environment-schema", std::to_string(EnvironmentProductSchemaVersion));
		Builder.UpdateText("environment-face-size", std::to_string(Description.EnvironmentFaceSize));
		Builder.UpdateText("irradiance-face-size", std::to_string(Description.IrradianceFaceSize));
		Builder.UpdateText("specular-face-size", std::to_string(Description.SpecularFaceSize));
		Builder.UpdateText("brdf-lut-size", std::to_string(Description.BrdfLutSize));
		Builder.UpdateText("irradiance-samples", std::to_string(Description.IrradianceSamples));
		Builder.UpdateText("prefilter-samples", std::to_string(Description.PrefilterSamples));
		Builder.UpdateText("brdf-samples", std::to_string(Description.BrdfSamples));
		return { "environment-lighting", Builder.Finish(), EnvironmentProductSchemaVersion };
	}

	DerivedDataArtifactKey Detail::EnvironmentLightingArtifactCodec::MakeBrdfLutKey(
		const EnvironmentProcessingDesc& Description)
	{
		AssetFingerprintBuilder Builder("PulseForge.EnvironmentBrdfRecipe.v1");
		Builder.UpdateText("algorithm", std::to_string(EnvironmentAlgorithmVersion));
		Builder.UpdateText("codec-schema", std::to_string(BrdfCodecVersion));
		Builder.UpdateText("size", std::to_string(Description.BrdfLutSize));
		Builder.UpdateText("samples", std::to_string(Description.BrdfSamples));
		return { "environment-brdf-lut", Builder.Finish(), BrdfCodecVersion };
	}

	std::expected<std::vector<std::byte>, std::string> Detail::EnvironmentLightingArtifactCodec::EncodeEnvironment(
		const EnvironmentLightingData& Data,
		const EnvironmentProcessingDesc& Description)
	{
		if (!IsValidEnvironmentDescription(Description))
			return std::unexpected("Environment artifact processing dimensions or samples exceed bounds");
		const uint32_t MipCount = GetMipCount(Description.SpecularFaceSize);
		if (Data.PrefilteredSpecular.size() != MipCount)
			return std::unexpected("Environment artifact has an invalid specular mip count");
		try
		{
			const uint64_t FloatCount = GetEnvironmentFloatCount(Description);
			std::vector<std::byte> Payload;
			Payload.reserve(8 * sizeof(uint32_t) + static_cast<size_t>(FloatCount) * sizeof(float));
			AppendU32(Payload, EnvironmentCodecVersion);
			AppendU32(Payload, Description.EnvironmentFaceSize);
			AppendU32(Payload, Description.IrradianceFaceSize);
			AppendU32(Payload, Description.SpecularFaceSize);
			AppendU32(Payload, Description.BrdfLutSize);
			AppendU32(Payload, Description.IrradianceSamples);
			AppendU32(Payload, Description.PrefilterSamples);
			AppendU32(Payload, Description.BrdfSamples);
			AppendU32(Payload, MipCount);
			if (!AppendLevelFloats(Payload, Data.Environment, Description.EnvironmentFaceSize) ||
				!AppendLevelFloats(Payload, Data.DiffuseIrradiance, Description.IrradianceFaceSize))
				return std::unexpected("Environment or irradiance cube has an invalid layout or non-finite channel");
			uint32_t MipSize = Description.SpecularFaceSize;
			for (const FloatCubeLevel& Level : Data.PrefilteredSpecular)
			{
				if (!AppendLevelFloats(Payload, Level, MipSize))
					return std::unexpected("Specular cube mip has an invalid layout or non-finite channel");
				MipSize = std::max(1u, MipSize / 2);
			}
			return Payload;
		}
		catch (const std::bad_alloc&)
		{
			return std::unexpected("Could not allocate bounded environment artifact payload");
		}
	}

	std::expected<EnvironmentLightingData, std::string> Detail::EnvironmentLightingArtifactCodec::DecodeEnvironment(
		std::span<const std::byte> Payload,
		const EnvironmentProcessingDesc& Description)
	{
		if (!IsValidEnvironmentDescription(Description))
			return std::unexpected("Environment artifact processing dimensions or samples exceed bounds");
		const uint64_t ExpectedBytes = 9 * sizeof(uint32_t) + GetEnvironmentFloatCount(Description) * sizeof(float);
		if (Payload.size() != ExpectedBytes)
			return std::unexpected("Environment artifact payload size does not match its declared processing layout");
		size_t Offset = 0;
		std::array<uint32_t, 9> Fields{};
		for (uint32_t& Field : Fields)
			if (!ReadU32(Payload, Offset, Field))
				return std::unexpected("Environment artifact header is truncated");
		const std::array<uint32_t, 9> ExpectedFields{
			EnvironmentCodecVersion, Description.EnvironmentFaceSize, Description.IrradianceFaceSize,
			Description.SpecularFaceSize, Description.BrdfLutSize, Description.IrradianceSamples,
			Description.PrefilterSamples, Description.BrdfSamples, GetMipCount(Description.SpecularFaceSize)
		};
		if (Fields != ExpectedFields)
			return std::unexpected("Environment artifact codec or processing schema is incompatible");

		EnvironmentLightingData Data;
		if (!ReadLevelFloats(Payload, Offset, Data.Environment, Description.EnvironmentFaceSize) ||
			!ReadLevelFloats(Payload, Offset, Data.DiffuseIrradiance, Description.IrradianceFaceSize))
			return std::unexpected("Environment artifact has an invalid cube layout or non-finite channel");
		try
		{
			Data.PrefilteredSpecular.reserve(Fields[8]);
			uint32_t MipSize = Description.SpecularFaceSize;
			for (uint32_t Mip = 0; Mip < Fields[8]; ++Mip)
			{
				FloatCubeLevel& Level = Data.PrefilteredSpecular.emplace_back();
				if (!ReadLevelFloats(Payload, Offset, Level, MipSize))
					return std::unexpected("Environment artifact has an invalid specular mip layout or channel");
				MipSize = std::max(1u, MipSize / 2);
			}
		}
		catch (const std::bad_alloc&)
		{
			return std::unexpected("Could not allocate bounded environment artifact result");
		}
		if (Offset != Payload.size())
			return std::unexpected("Environment artifact contains trailing data");
		return Data;
	}

	std::expected<std::vector<std::byte>, std::string> Detail::EnvironmentLightingArtifactCodec::EncodeBrdfLut(
		const ImportedFloatImage& Image,
		uint32_t Samples)
	{
		if (Image.Width == 0 || Image.Width > 2048 || Image.Height != Image.Width || Samples == 0 || Samples > 4096 ||
			Image.RGBA32FPixels.size() != static_cast<uint64_t>(Image.Width) * Image.Height * 4 ||
			Image.RGBA32FPixels.size() * sizeof(float) > MaximumCacheBytes || !IsFiniteImage(Image))
			return std::unexpected("BRDF LUT dimensions, sample count, or pixels are invalid");
		try
		{
			std::vector<std::byte> Payload;
			Payload.reserve(4 * sizeof(uint32_t) + Image.RGBA32FPixels.size() * sizeof(float));
			AppendU32(Payload, BrdfCodecVersion);
			AppendU32(Payload, Image.Width);
			AppendU32(Payload, Image.Height);
			AppendU32(Payload, Samples);
			for (float Value : Image.RGBA32FPixels)
				AppendFloat(Payload, Value);
			return Payload;
		}
		catch (const std::bad_alloc&)
		{
			return std::unexpected("Could not allocate bounded BRDF LUT artifact payload");
		}
	}

	std::expected<ImportedFloatImage, std::string> Detail::EnvironmentLightingArtifactCodec::DecodeBrdfLut(
		std::span<const std::byte> Payload,
		uint32_t ExpectedSize,
		uint32_t ExpectedSamples)
	{
		if (ExpectedSize == 0 || ExpectedSize > 2048 || ExpectedSamples == 0 || ExpectedSamples > 4096)
			return std::unexpected("Requested BRDF LUT dimensions or sample count exceed bounds");
		const uint64_t FloatCount = static_cast<uint64_t>(ExpectedSize) * ExpectedSize * 4;
		if (FloatCount * sizeof(float) > MaximumCacheBytes || Payload.size() != 4 * sizeof(uint32_t) + FloatCount * sizeof(float))
			return std::unexpected("BRDF LUT artifact payload size is invalid");
		size_t Offset = 0;
		std::array<uint32_t, 4> Fields{};
		for (uint32_t& Field : Fields)
			if (!ReadU32(Payload, Offset, Field))
				return std::unexpected("BRDF LUT artifact header is truncated");
		if (Fields != std::array<uint32_t, 4>{ BrdfCodecVersion, ExpectedSize, ExpectedSize, ExpectedSamples })
			return std::unexpected("BRDF LUT artifact schema does not match the request");
		ImportedFloatImage Image;
		Image.Width = ExpectedSize;
		Image.Height = ExpectedSize;
		try
		{
			Image.RGBA32FPixels.resize(static_cast<size_t>(FloatCount));
		}
		catch (const std::bad_alloc&)
		{
			return std::unexpected("Could not allocate bounded BRDF LUT artifact result");
		}
		for (float& Value : Image.RGBA32FPixels)
			if (!ReadFloat(Payload, Offset, Value))
				return std::unexpected("BRDF LUT artifact has a non-finite or truncated channel");
		return Image;
	}


	EnvironmentLightingCache::EnvironmentLightingCache(
		RenderDevice& Runtime, std::filesystem::path ProjectRoot, const AssetRegistry& Registry,
		DiagnosticRecorder* Diagnostics, DerivedDataArtifactStoreOptions ArtifactStoreOptions)
		: m_Runtime(Runtime), m_ProjectRoot(std::move(ProjectRoot)),
		  m_RegistryOwner(std::make_shared<const AssetRegistry>(Registry)), m_Registry(m_RegistryOwner.get()), m_Diagnostics(Diagnostics),
		  m_OwnedPreparationService(std::make_unique<AssetPreparationService>(MakeCompatibilityPreparationLimits())),
		  m_Preparation(m_OwnedPreparationService.get()),
		  m_OwnedArtifactStore(std::make_unique<DerivedDataArtifactStore>(
			  ResolveArtifactStoreOptions(m_ProjectRoot, std::move(ArtifactStoreOptions)))),
		  m_ArtifactStore(m_OwnedArtifactStore.get()), m_SessionKey(MakeCompatibilityPreparationSession(m_ProjectRoot)),
		  m_Processing(GetProcessingDescription())
	{
	}

	EnvironmentLightingCache::EnvironmentLightingCache(
		RenderDevice& Runtime, std::filesystem::path ProjectRoot, const AssetRegistry& Registry,
		DerivedDataArtifactStore& ArtifactStore, DiagnosticRecorder* Diagnostics)
		: m_Runtime(Runtime), m_ProjectRoot(std::move(ProjectRoot)),
		  m_RegistryOwner(std::make_shared<const AssetRegistry>(Registry)), m_Registry(m_RegistryOwner.get()), m_Diagnostics(Diagnostics),
		  m_OwnedPreparationService(std::make_unique<AssetPreparationService>(MakeCompatibilityPreparationLimits())),
		  m_Preparation(m_OwnedPreparationService.get()), m_ArtifactStore(&ArtifactStore),
		  m_SessionKey(MakeCompatibilityPreparationSession(m_ProjectRoot)), m_Processing(GetProcessingDescription())
	{
	}

	EnvironmentLightingCache::EnvironmentLightingCache(
		RenderDevice& Runtime, std::filesystem::path ProjectRoot, const AssetRegistry& Registry,
		AssetPreparationService& Preparation, AssetUploadBudget* UploadBudget, DerivedDataArtifactStore& ArtifactStore,
		std::string SessionKey, DiagnosticRecorder* Diagnostics, EnvironmentProcessingDesc Processing)
		: m_Runtime(Runtime), m_ProjectRoot(std::move(ProjectRoot)),
		  m_RegistryOwner(std::make_shared<const AssetRegistry>(Registry)), m_Registry(m_RegistryOwner.get()), m_Diagnostics(Diagnostics),
		  m_Preparation(&Preparation), m_UploadBudget(UploadBudget), m_ArtifactStore(&ArtifactStore),
		  m_SessionKey(std::move(SessionKey)), m_Processing(Processing)
	{
	}

	EnvironmentLightingCache::EnvironmentLightingCache(
		RenderDevice& Runtime, std::filesystem::path ProjectRoot, std::shared_ptr<const AssetRegistry> Registry,
		AssetPreparationService& Preparation, AssetUploadBudget* UploadBudget, DerivedDataArtifactStore& ArtifactStore,
		std::string SessionKey, DiagnosticRecorder* Diagnostics, EnvironmentProcessingDesc Processing)
		: m_Runtime(Runtime), m_ProjectRoot(std::move(ProjectRoot)), m_RegistryOwner(std::move(Registry)),
		  m_Registry(m_RegistryOwner.get()), m_Diagnostics(Diagnostics), m_Preparation(&Preparation),
		  m_UploadBudget(UploadBudget), m_ArtifactStore(&ArtifactStore), m_SessionKey(std::move(SessionKey)),
		  m_Processing(Processing)
	{
	}

	EnvironmentLightingCache::~EnvironmentLightingCache()
	{
		Clear();
		if (m_OwnedPreparationService)
			m_OwnedPreparationService->Shutdown();
	}

	void EnvironmentLightingCache::Clear() noexcept
	{
		uint64_t RetiredCount = 0;
		for (const auto& [Asset, State] : m_Entries)
		{
			(void)Asset;
			if (State->GpuTextures) ++RetiredCount;
		}
		m_Entries.clear();
		if (m_Diagnostics && RetiredCount > 0)
			(void)m_Diagnostics->RecordEvent(DiagnosticEventKind::Resource, "Environment cache resources retired",
				"EnvironmentLightingCache", "objects", RetiredCount);
		if (m_Diagnostics && m_CacheHits > 0)
			(void)m_Diagnostics->RecordEvent(DiagnosticEventKind::Counter, "Environment cache hits",
				"EnvironmentLightingCache", "lookups", m_CacheHits);
		if (m_Diagnostics && m_CacheMisses > 0)
			(void)m_Diagnostics->RecordEvent(DiagnosticEventKind::Counter, "Environment cache misses",
				"EnvironmentLightingCache", "lookups", m_CacheMisses);
		m_CacheHits = 0;
		m_CacheMisses = 0;
	}

	void EnvironmentLightingCache::SetSessionKey(std::string SessionKey)
	{
		if (SessionKey == m_SessionKey) return;
		m_SessionKey = std::move(SessionKey);
		for (auto& [Asset, State] : m_Entries)
		{
			(void)Asset;
			State->Pending.reset();
			State->Error.clear();
			State->UploadFailed = false;
			State->AdmissionDeferred = false;
			State->SessionKey.clear();
			State->DiagnosticSessionId = 0;
		}
	}

	void EnvironmentLightingCache::SetRegistrySnapshot(std::shared_ptr<const AssetRegistry> Registry) noexcept
	{
		if (!Registry)
			return;
		m_RegistryOwner = std::move(Registry);
		m_Registry = m_RegistryOwner.get();
	}

	std::expected<std::optional<std::reference_wrapper<const EnvironmentLightingTextures>>, EnvironmentLightingCacheError>
	EnvironmentLightingCache::GetOrLoad(const AssetID& Asset)
	{
		if (m_SessionKey.empty())
		{
			const auto Existing = m_Entries.find(Asset);
			if (Existing != m_Entries.end() && Existing->second->GpuTextures)
				return std::cref(*Existing->second->GpuTextures);
			return std::unexpected(MakeError(Asset, "Asynchronous environment loading requires a non-empty project session identity"));
		}
		auto Found = m_Entries.find(Asset);
		if (Found == m_Entries.end())
		{
			auto NewEntry = std::make_shared<Entry>();
			Found = m_Entries.emplace(Asset, std::move(NewEntry)).first;
		}
		else
			++m_CacheHits;

		Entry& State = *Found->second;
		if (State.SessionKey != m_SessionKey)
		{
			State.Pending.reset();
			State.Error.clear();
			State.UploadFailed = false;
			State.AdmissionDeferred = false;
			State.SessionKey = m_SessionKey;
			State.DiagnosticSessionId = 0;
			if (m_Diagnostics)
			{
				uint64_t Start = 0;
				(void)m_Diagnostics->BeginDurationSession(State.DiagnosticSessionId, Start);
			}
		}
		if (State.GpuTextures && !State.Pending && State.PublishedSessionKey == m_SessionKey)
			return std::cref(*State.GpuTextures);
		if (!State.Pending && !State.UploadFailed)
		{
			auto RegistrySnapshot = m_RegistryOwner;
			const std::filesystem::path Root = m_ProjectRoot;
			DerivedDataArtifactStore* Store = m_ArtifactStore;
			const EnvironmentProcessingDesc Processing = m_Processing;
			const std::string Recipe = "environment-lighting-v1/" + Asset.ToString() + "/" +
				std::to_string(Processing.EnvironmentFaceSize) + "/" + std::to_string(Processing.IrradianceFaceSize) + "/" +
				std::to_string(Processing.SpecularFaceSize) + "/" + std::to_string(Processing.BrdfLutSize) + "/" +
				std::to_string(Processing.IrradianceSamples) + "/" + std::to_string(Processing.PrefilterSamples) + "/" +
				std::to_string(Processing.BrdfSamples);
			auto Requested = m_Preparation->Request({ m_SessionKey, Recipe }, AssetPreparationPriority::Interactive,
				256ull * 1024ull * 1024ull,
				[Asset, Root, RegistrySnapshot, Store, Processing](std::stop_token Token)
					-> std::expected<AssetPreparationService::CpuPayload, AssetPreparationError>
				{
					auto Product = Detail::PrepareEnvironmentCpuProduct(
						Asset, Root, *RegistrySnapshot, Store, Processing, Token);
					if (!Product)
						return std::unexpected(std::move(Product.error()));
					const size_t Retained = GetPreparedEnvironmentRetainedBytes(Product->Data);
					return AssetPreparationService::CpuPayload(
						MakeAssetPreparationValue(std::move(*Product), Retained));
				});
			if (!Requested)
			{
				if (Requested.error().Code == AssetPreparationErrorCode::WorkingSetLimit ||
					Requested.error().Code == AssetPreparationErrorCode::QueueFull)
				{
					State.AdmissionDeferred = true;
					State.Error.clear();
					if (State.GpuTextures)
						return std::cref(*State.GpuTextures);
					return std::optional<std::reference_wrapper<const EnvironmentLightingTextures>>{};
				}
				State.AdmissionDeferred = false;
				State.Error = Requested.error().Message;
				State.UploadFailed = true;
				State.SessionKey.clear();
				if (State.GpuTextures) return std::cref(*State.GpuTextures);
				return std::unexpected(MakeError(Asset, State.Error));
			}
			State.Pending.emplace(std::move(*Requested));
			State.AdmissionDeferred = false;
			++m_CacheMisses;
		}
		if (!State.Pending)
		{
			if (State.GpuTextures) return std::cref(*State.GpuTextures);
			return std::unexpected(MakeError(Asset, State.Error.empty() ? "Environment preparation is pending" : State.Error));
		}

		const AssetPreparationSnapshot Prepared = State.Pending->Poll();
		switch (Prepared.Status)
		{
			case AssetPreparationStatus::Queued:
			case AssetPreparationStatus::Processing:
				if (State.GpuTextures) return std::cref(*State.GpuTextures);
				return std::optional<std::reference_wrapper<const EnvironmentLightingTextures>>{};
			case AssetPreparationStatus::CpuReady:
			{
				const auto* Product = Prepared.CpuPayload
					? dynamic_cast<const AssetPreparationValue<Detail::EnvironmentCpuPreparationResult>*>(Prepared.CpuPayload.get()) : nullptr;
				if (!Product)
				{
					State.Error = "Environment preparation returned an unexpected CPU product type";
					State.UploadFailed = true;
					State.Pending.reset();
					if (State.GpuTextures) return std::cref(*State.GpuTextures);
					return std::unexpected(MakeError(Asset, State.Error));
				}
				if (m_UploadBudget && !m_UploadBudget->TryReserve(Prepared.CpuPayload->GetRetainedBytes()))
				{
					if (State.GpuTextures) return std::cref(*State.GpuTextures);
					return std::optional<std::reference_wrapper<const EnvironmentLightingTextures>>{};
				}
				if (m_Diagnostics)
				{
					const auto& T = Product->Get();
					if (T.ImportEndTimestampNanoseconds > T.ImportStartTimestampNanoseconds)
						(void)m_Diagnostics->RecordDurationForSession(State.DiagnosticSessionId,
							T.ImportStartTimestampNanoseconds, T.ImportEndTimestampNanoseconds,
							"Environment source import", "EnvironmentLightingCache");
					if (T.DerivedCacheReadEndTimestampNanoseconds > T.DerivedCacheReadStartTimestampNanoseconds)
						(void)m_Diagnostics->RecordDurationForSession(State.DiagnosticSessionId,
							T.DerivedCacheReadStartTimestampNanoseconds, T.DerivedCacheReadEndTimestampNanoseconds,
							"Environment derived cache read", "EnvironmentLightingCache");
					if (T.PreprocessEndTimestampNanoseconds > T.PreprocessStartTimestampNanoseconds)
						(void)m_Diagnostics->RecordDurationForSession(State.DiagnosticSessionId,
							T.PreprocessStartTimestampNanoseconds, T.PreprocessEndTimestampNanoseconds,
							"Environment CPU preprocessing", "EnvironmentLightingCache");
				}
				ScopedDiagnosticTimer UploadTimer(m_Diagnostics, "Environment GPU upload", "EnvironmentLightingCache");
				auto Uploaded = UploadProcessedData(Asset, Product->Get().Data);
				if (!Uploaded)
				{
					State.Error = Uploaded.error().Message;
					State.UploadFailed = true;
					State.Pending.reset();
					if (State.GpuTextures) return std::cref(*State.GpuTextures);
					return std::unexpected(std::move(Uploaded.error()));
				}
				State.GpuTextures.emplace(std::move(*Uploaded));
				++State.ResourceRevision;
				State.PublishedSessionKey = m_SessionKey;
				State.Pending.reset();
				State.UploadFailed = false;
				State.Error.clear();
				if (m_Diagnostics)
					(void)m_Diagnostics->RecordEvent(DiagnosticEventKind::Resource, "Environment cache resource created",
						"EnvironmentLightingCache", "objects", 1);
				return std::cref(*State.GpuTextures);
			}
			case AssetPreparationStatus::Failed:
				State.Error = Prepared.Error ? Prepared.Error->Message : "Environment CPU preparation failed";
				State.UploadFailed = true;
				State.Pending.reset();
				if (State.GpuTextures) return std::cref(*State.GpuTextures);
				return std::unexpected(MakeError(Asset, State.Error));
			case AssetPreparationStatus::Cancelled:
				State.Error = Prepared.Error ? Prepared.Error->Message : "Environment CPU preparation was cancelled";
				State.UploadFailed = true;
				State.Pending.reset();
				if (State.GpuTextures) return std::cref(*State.GpuTextures);
				return std::unexpected(MakeError(Asset, State.Error));
		}
		return std::unexpected(MakeError(Asset, "Environment cache reached an invalid preparation state"));
	}

	bool EnvironmentLightingCache::AdvancePending()
	{
		std::vector<AssetID> Assets;
		Assets.reserve(m_Entries.size());
		for (const auto& [Asset, State] : m_Entries)
		{
			(void)State;
			Assets.push_back(Asset);
		}

		bool PublishedReplacement = false;
		for (const AssetID& Asset : Assets)
		{
			const auto Found = m_Entries.find(Asset);
			if (Found == m_Entries.end())
				continue;
			const uint64_t PreviousRevision = Found->second->ResourceRevision;
			(void)GetOrLoad(Asset);
			const auto Updated = m_Entries.find(Asset);
			PublishedReplacement |= Updated != m_Entries.end() && Updated->second->ResourceRevision != PreviousRevision;
		}
		return PublishedReplacement;
	}

	uint64_t EnvironmentLightingCache::GetResourceRevision(const AssetID& Asset) const noexcept
	{
		const auto Found = m_Entries.find(Asset);
		return Found == m_Entries.end() ? 0 : Found->second->ResourceRevision;
	}

	bool EnvironmentLightingCache::HasPendingWork() const noexcept
	{
		return std::ranges::any_of(m_Entries, [this](const auto& Pair)
		{
			const auto& State = *Pair.second;
			return State.Pending.has_value() || State.AdmissionDeferred;
		});
	}

	std::expected<Detail::EnvironmentCpuPreparationResult, AssetPreparationError> Detail::PrepareEnvironmentCpuProduct(
		AssetID Asset, std::filesystem::path ProjectRoot, const AssetRegistry& Registry,
		DerivedDataArtifactStore* ArtifactStore, EnvironmentProcessingDesc Processing, std::stop_token StopToken)
	{
		auto Fail = [](std::string Message)
			-> std::expected<EnvironmentCpuPreparationResult, AssetPreparationError>
		{
			return std::unexpected(AssetPreparationError{ AssetPreparationErrorCode::ProcessingFailed, std::move(Message) });
		};
		try
		{
			if (StopToken.stop_requested()) return Fail("Environment preparation cancelled before processing");
			EnvironmentCpuPreparationResult Product;
			Product.DerivedCacheReadStartTimestampNanoseconds = GetMonotonicTimestampNanoseconds();
			auto Source = ObserveEnvironmentSource(Asset, ProjectRoot, Registry);
			if (!Source) return Fail("Environment source fingerprint failed: " + Source.error());
			const DerivedDataArtifactKey Key =
				Detail::EnvironmentLightingArtifactCodec::MakeEnvironmentKey(Asset, Source->Content, Processing);
			std::optional<EnvironmentLightingData> Data;
			std::optional<AssetContentFingerprint> RejectedEnvironmentEntry;
			bool CacheHit = false;
			bool BrdfCacheHit = false;
			if (ArtifactStore)
			{
				const auto Artifact = ArtifactStore->Read(Key);
				if (Artifact.Status == DerivedDataArtifactReadStatus::Hit)
				{
					auto Decoded = Detail::EnvironmentLightingArtifactCodec::DecodeEnvironment(Artifact.Payload, Processing);
					if (Decoded) { Data = std::move(*Decoded); CacheHit = true; }
					else
					{
						RejectedEnvironmentEntry = Artifact.EntryChecksum;
						PF_WARN("Environment artifact payload was invalid for {}: {}", Asset.ToString(), Decoded.error());
					}
				}
				else if (Artifact.Status == DerivedDataArtifactReadStatus::Corrupt ||
					Artifact.Status == DerivedDataArtifactReadStatus::Incompatible)
					PF_WARN("Environment artifact for {} must be rebuilt: {}", Asset.ToString(), Artifact.Message);
			}
			Product.DerivedCacheReadEndTimestampNanoseconds = GetMonotonicTimestampNanoseconds();
			if (StopToken.stop_requested()) return Fail("Environment preparation cancelled during cache lookup");

			const DerivedDataArtifactKey BrdfKey = Detail::EnvironmentLightingArtifactCodec::MakeBrdfLutKey(Processing);
			std::optional<ImportedFloatImage> Brdf;
			std::optional<AssetContentFingerprint> RejectedBrdfEntry;
			if (ArtifactStore)
			{
				const auto Artifact = ArtifactStore->Read(BrdfKey);
				if (Artifact.Status == DerivedDataArtifactReadStatus::Hit)
				{
					auto Decoded = Detail::EnvironmentLightingArtifactCodec::DecodeBrdfLut(
						Artifact.Payload, Processing.BrdfLutSize, Processing.BrdfSamples);
					if (Decoded) { Brdf = std::move(*Decoded); BrdfCacheHit = true; }
					else
					{
						RejectedBrdfEntry = Artifact.EntryChecksum;
						PF_WARN("Shared BRDF artifact payload was invalid: {}", Decoded.error());
					}
				}
				else if (Artifact.Status == DerivedDataArtifactReadStatus::Corrupt ||
					Artifact.Status == DerivedDataArtifactReadStatus::Incompatible)
					PF_WARN("Shared BRDF artifact must be rebuilt: {}", Artifact.Message);
			}
			const std::filesystem::path LegacyBrdf = GetBrdfCachePath(ProjectRoot, Processing.BrdfLutSize, Processing.BrdfSamples);
			if (!Brdf)
			{
				Brdf = ReadBrdfCache(LegacyBrdf, Processing.BrdfLutSize, Processing.BrdfSamples);
				BrdfCacheHit = Brdf.has_value();
				if (Brdf && ArtifactStore)
					if (auto Encoded = Detail::EnvironmentLightingArtifactCodec::EncodeBrdfLut(*Brdf, Processing.BrdfSamples))
					{
						const auto Stored = RejectedBrdfEntry
							? ArtifactStore->ReplaceRejectedProduct(BrdfKey, *RejectedBrdfEntry, *Encoded)
							: ArtifactStore->Write(BrdfKey, *Encoded);
						if ((Stored.Status == DerivedDataArtifactWriteStatus::Unavailable ||
							Stored.Status == DerivedDataArtifactWriteStatus::Rejected) && ArtifactStore->GetOptions().Enabled)
							PF_WARN("Could not migrate the read-only BRDF cache: {}", Stored.Message);
					}
			}
			if (!Brdf)
			{
				Product.RanBrdfPreprocessing = true;
				auto Generated = GenerateBrdfIntegrationLut(Processing.BrdfLutSize, Processing.BrdfSamples, StopToken);
				if (!Generated) return Fail(Generated.error().Message);
				Brdf = std::move(*Generated);
				if (ArtifactStore)
					if (auto Encoded = Detail::EnvironmentLightingArtifactCodec::EncodeBrdfLut(*Brdf, Processing.BrdfSamples))
					{
						const auto Stored = RejectedBrdfEntry
							? ArtifactStore->ReplaceRejectedProduct(BrdfKey, *RejectedBrdfEntry, *Encoded)
							: ArtifactStore->Write(BrdfKey, *Encoded);
						if ((Stored.Status == DerivedDataArtifactWriteStatus::Unavailable ||
							Stored.Status == DerivedDataArtifactWriteStatus::Rejected) && ArtifactStore->GetOptions().Enabled)
							PF_WARN("Could not persist shared BRDF artifact: {}", Stored.Message);
					}
			}

			std::optional<std::vector<std::byte>> ToStore;
			if (!Data)
			{
				const auto LegacyPath = GetEnvironmentCachePath(ProjectRoot, Asset, Source->LegacyFingerprint, Processing);
				Data = ReadEnvironmentCache(LegacyPath, Asset, Source->LegacyFingerprint, Processing);
				if (Data)
				{
					CacheHit = true;
					if (auto Encoded = Detail::EnvironmentLightingArtifactCodec::EncodeEnvironment(*Data, Processing))
						ToStore = std::move(*Encoded);
				}
				else
				{
					Product.ImportStartTimestampNanoseconds = GetMonotonicTimestampNanoseconds();
					auto Imported = ImageAssetImporter::ImportRGBA32F(Asset, ProjectRoot, Registry);
					Product.ImportEndTimestampNanoseconds = GetMonotonicTimestampNanoseconds();
					if (!Imported) return Fail("HDR image import failed: " + Imported.error().Message);
					if (Imported->SourceFingerprint != Source->LegacyFingerprint)
						return Fail("Environment source changed between fingerprinting and HDR decode");
					Product.PreprocessStartTimestampNanoseconds = GetMonotonicTimestampNanoseconds();
					Product.RanEnvironmentPreprocessing = true;
					auto Processed = ProcessEnvironmentImage(*Imported, Processing, StopToken, &*Brdf);
					if (!Processed) return Fail("Environment preprocessing failed: " + Processed.error().Message);
					Data = std::move(*Processed);
					Product.PreprocessEndTimestampNanoseconds = GetMonotonicTimestampNanoseconds();
					if (auto Encoded = Detail::EnvironmentLightingArtifactCodec::EncodeEnvironment(*Data, Processing))
						ToStore = std::move(*Encoded);
				}
			}
			if (!Data) return Fail("Environment CPU preparation produced no data");
			if (StopToken.stop_requested()) return Fail("Environment preparation cancelled before publication");
			auto Current = ObserveEnvironmentSource(Asset, ProjectRoot, Registry);
			if (!Current || Current->Content != Source->Content)
				return Fail("Environment source changed during preparation; stale product discarded");
			if (ToStore && ArtifactStore)
			{
				const auto Stored = RejectedEnvironmentEntry
					? ArtifactStore->ReplaceRejectedProduct(Key, *RejectedEnvironmentEntry, *ToStore)
					: ArtifactStore->Write(Key, *ToStore);
				if ((Stored.Status == DerivedDataArtifactWriteStatus::Unavailable ||
					Stored.Status == DerivedDataArtifactWriteStatus::Rejected) && ArtifactStore->GetOptions().Enabled)
					PF_WARN("Could not persist environment artifact for {}: {}", Asset.ToString(), Stored.Message);
			}
			Product.EnvironmentArtifactHit = CacheHit;
			Product.BrdfArtifactHit = BrdfCacheHit;
			Product.Source = CacheHit
				? (ToStore ? EnvironmentCpuProductSource::LegacyArtifact : EnvironmentCpuProductSource::DerivedArtifact)
				: EnvironmentCpuProductSource::Processed;
			Data->BrdfIntegrationLut = std::move(*Brdf);
			Product.Data = std::move(*Data);
			PF_INFO("Environment derived data {} for {}", CacheHit ? "loaded from cache" : "generated", Asset.ToString());
			return Product;
		}
		catch (const std::exception& Exception)
		{
			return Fail(std::string("Background environment preparation failed: ") + Exception.what());
		}
		catch (...)
		{
			return Fail("Background environment preparation failed with an unknown error");
		}
	}

	std::expected<EnvironmentLightingTextures, EnvironmentLightingCacheError> EnvironmentLightingCache::UploadProcessedData(
		const AssetID& Asset,
		const EnvironmentLightingData& Data)
	{
		Detail::ScopedProfileTimer UploadTimer("HDR GPU texture uploads (renderer thread)");
		try
		{
			EnvironmentLightingTextures Textures;
			auto Environment = CreateCubeTexture(m_Runtime, Data.Environment, 1, {}, "PulseForge environment cubemap");
			if (!Environment)
				return std::unexpected(MakeError(Asset, "Environment cubemap creation failed: " + Environment.error().Message));
			Textures.Environment = std::move(*Environment);

			auto Irradiance = CreateCubeTexture(m_Runtime, Data.DiffuseIrradiance, 1, {}, "PulseForge diffuse irradiance cubemap");
			if (!Irradiance)
				return std::unexpected(MakeError(Asset, "Irradiance cubemap creation failed: " + Irradiance.error().Message));
			Textures.DiffuseIrradiance = std::move(*Irradiance);

			auto Specular = CreateCubeTexture(m_Runtime, Data.PrefilteredSpecular.front(),
				static_cast<uint32_t>(Data.PrefilteredSpecular.size()), Data.PrefilteredSpecular,
				"PulseForge prefiltered specular cubemap");
			if (!Specular)
				return std::unexpected(MakeError(Asset, "Specular cubemap creation failed: " + Specular.error().Message));
			Textures.PrefilteredSpecular = std::move(*Specular);

			TextureDesc BrdfDescription;
			BrdfDescription.Width = Data.BrdfIntegrationLut.Width;
			BrdfDescription.Height = Data.BrdfIntegrationLut.Height;
			BrdfDescription.Format = TextureFormat::RGBA32_Float;
			BrdfDescription.DebugName = "PulseForge shared split-sum BRDF integration LUT";
			const std::span<const float> BrdfPixels(Data.BrdfIntegrationLut.RGBA32FPixels);
			auto Brdf = m_Runtime.CreateTexture(BrdfDescription, std::as_bytes(BrdfPixels));
			if (!Brdf)
				return std::unexpected(MakeError(Asset, "BRDF integration LUT creation failed: " + Brdf.error().Message));
			Textures.BrdfIntegrationLut = std::move(*Brdf);
			return Textures;
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(MakeError(Asset, std::string("Environment GPU resource allocation failed: ") + Exception.what()));
		}
	}
}
