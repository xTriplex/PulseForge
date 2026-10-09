#include "Core/PulseForgePCH.h"
#include "Renderer/EnvironmentLightingCache.h"

#include "Assets/AssetPathResolver.h"
#include "Assets/ImageAssetImporter.h"
#include "Core/Application.h"
#include "Core/ScopedProfileTimer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <new>
#include <span>
#include <sstream>
#include <system_error>

namespace PulseForge
{
	namespace
	{
		constexpr uint32_t EnvironmentCacheVersion = 1;
		constexpr uint32_t EnvironmentAlgorithmVersion = 1;
		constexpr uint32_t BrdfCacheVersion = 1;
		constexpr uint64_t MaximumCacheBytes = 64ull * 1024ull * 1024ull;

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

		bool WriteTransactionally(
			const std::filesystem::path& Destination,
			const void* Header,
			size_t HeaderSize,
			std::span<const float> Pixels)
		{
			std::error_code Error;
			std::filesystem::create_directories(Destination.parent_path(), Error);
			if (Error)
				return false;
			std::filesystem::path Temporary = Destination;
			Temporary += ".tmp";
			std::filesystem::remove(Temporary, Error);
			Error.clear();
			{
				std::ofstream Output(Temporary, std::ios::binary | std::ios::trunc);
				if (!Output)
					return false;
				Output.write(static_cast<const char*>(Header), static_cast<std::streamsize>(HeaderSize));
				Output.write(reinterpret_cast<const char*>(Pixels.data()),
					static_cast<std::streamsize>(Pixels.size_bytes()));
				Output.flush();
				if (!Output)
				{
					Output.close();
					std::filesystem::remove(Temporary, Error);
					return false;
				}
			}
			std::filesystem::rename(Temporary, Destination, Error);
			if (Error)
			{
				std::filesystem::remove(Temporary, Error);
				return false;
			}
			return true;
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

		void WriteBrdfCache(const std::filesystem::path& Path, const ImportedFloatImage& Image, uint32_t Samples)
		{
			BrdfCacheHeader Header;
			Header.Size = Image.Width;
			Header.Samples = Samples;
			if (!WriteTransactionally(Path, &Header, sizeof(Header), Image.RGBA32FPixels))
				PF_WARN("Could not persist the generated BRDF LUT cache at {}", Path.string());
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

		void AppendLevelPixels(std::vector<float>& Destination, const FloatCubeLevel& Level)
		{
			for (const std::vector<float>& Face : Level.Faces)
				Destination.insert(Destination.end(), Face.begin(), Face.end());
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

		void WriteEnvironmentCache(
			const std::filesystem::path& Path,
			const AssetID& Asset,
			const std::array<uint64_t, 2>& Fingerprint,
			const EnvironmentProcessingDesc& Description,
			const EnvironmentLightingData& Data)
		{
			std::vector<float> Pixels;
			try
			{
				const uint64_t FloatCount = GetEnvironmentFloatCount(Description);
				if (FloatCount > MaximumCacheBytes / sizeof(float))
					return;
				Pixels.reserve(static_cast<size_t>(FloatCount));
				AppendLevelPixels(Pixels, Data.Environment);
				AppendLevelPixels(Pixels, Data.DiffuseIrradiance);
				for (const FloatCubeLevel& Level : Data.PrefilteredSpecular)
					AppendLevelPixels(Pixels, Level);
			}
			catch (const std::bad_alloc&)
			{
				PF_WARN("Could not allocate an environment derived-data cache buffer for {}", Asset.ToString());
				return;
			}

			EnvironmentCacheHeader Header;
			Header.FingerprintA = Fingerprint[0];
			Header.FingerprintB = Fingerprint[1];
			Header.EnvironmentFaceSize = Description.EnvironmentFaceSize;
			Header.IrradianceFaceSize = Description.IrradianceFaceSize;
			Header.SpecularFaceSize = Description.SpecularFaceSize;
			Header.BrdfLutSize = Description.BrdfLutSize;
			Header.IrradianceSamples = Description.IrradianceSamples;
			Header.PrefilterSamples = Description.PrefilterSamples;
			Header.BrdfSamples = Description.BrdfSamples;
			Header.SpecularMipCount = static_cast<uint32_t>(Data.PrefilteredSpecular.size());
			if (!WriteTransactionally(Path, &Header, sizeof(Header), Pixels))
				PF_WARN("Could not persist environment derived-data cache for {}", Asset.ToString());
		}

		std::expected<TextureHandle, TextureError> CreateCubeTexture(
			Application& Runtime,
			const FloatCubeLevel& Level,
			uint32_t MipLevels,
			std::span<const FloatCubeLevel> Mips,
			std::string DebugName)
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
	}

	EnvironmentLightingCache::EnvironmentLightingCache(
		Application& Runtime,
		std::filesystem::path ProjectRoot,
		const AssetRegistry& Registry)
		: m_Runtime(Runtime),
		  m_ProjectRoot(std::move(ProjectRoot)),
		  m_Registry(Registry),
		  m_Worker([this](std::stop_token StopToken) { WorkerMain(StopToken); })
	{
	}

	EnvironmentLightingCache::~EnvironmentLightingCache()
	{
		m_Worker.request_stop();
		m_QueueChanged.notify_all();
		if (m_Worker.joinable())
			m_Worker.join();
	}

	std::expected<std::optional<std::reference_wrapper<const EnvironmentLightingTextures>>, EnvironmentLightingCacheError>
	EnvironmentLightingCache::GetOrLoad(const AssetID& Asset)
	{
		auto Found = m_Entries.find(Asset);
		if (Found == m_Entries.end())
		{
			auto NewEntry = std::make_unique<EnvironmentLightingCache::Entry>();
			EnvironmentLightingCache::Entry* EntryPointer = NewEntry.get();
			Found = m_Entries.emplace(Asset, std::move(NewEntry)).first;
			{
				std::lock_guard QueueLock(m_QueueMutex);
				m_Requests.push_back(Request{ Asset, m_ProjectRoot, m_Registry, EntryPointer });
			}
			m_QueueChanged.notify_one();
		}

		Entry& State = *Found->second;
		std::optional<EnvironmentLightingData> CpuData;
		{
			std::lock_guard EntryLock(State.Mutex);
			switch (State.State)
			{
				case EntryState::Queued:
				case EntryState::Processing:
				case EntryState::Uploading:
					return std::optional<std::reference_wrapper<const EnvironmentLightingTextures>>{};
				case EntryState::Ready:
					return std::cref(*State.GpuTextures);
				case EntryState::Failed:
					return std::unexpected(MakeError(Asset, State.Error));
				case EntryState::CpuReady:
					State.State = EntryState::Uploading;
					CpuData = std::move(State.CpuData);
					break;
			}
		}

		Detail::ScopedProfileTimer UploadTimer("Environment GPU texture creation/upload");
		auto Uploaded = UploadProcessedData(Asset, std::move(*CpuData));
		if (!Uploaded)
		{
			std::lock_guard EntryLock(State.Mutex);
			State.Error = Uploaded.error().Message;
			State.State = EntryState::Failed;
			return std::unexpected(std::move(Uploaded.error()));
		}
		{
			std::lock_guard EntryLock(State.Mutex);
			State.GpuTextures.emplace(std::move(*Uploaded));
			State.State = EntryState::Ready;
		}
		return std::cref(*State.GpuTextures);
	}

	void EnvironmentLightingCache::WorkerMain(std::stop_token StopToken) noexcept
	{
		while (!StopToken.stop_requested())
		{
			Request Work;
			{
				std::unique_lock QueueLock(m_QueueMutex);
				m_QueueChanged.wait(QueueLock, StopToken, [this] { return !m_Requests.empty(); });
				if (StopToken.stop_requested())
					return;
				Work = std::move(m_Requests.front());
				m_Requests.pop_front();
			}
			{
				std::lock_guard EntryLock(Work.Destination->Mutex);
				Work.Destination->State = EntryState::Processing;
			}

			try
			{
				const EnvironmentProcessingDesc Processing = GetProcessingDescription();
				std::expected<ImportedFloatImage, ImageAssetImportError> Imported = std::unexpected(
					ImageAssetImportError{ ImageAssetImportErrorCode::InvalidImage, {}, "HDR import has not started" });
				{
					Detail::ScopedProfileTimer Timer("HDR file decode (background)");
					Imported = ImageAssetImporter::ImportRGBA32F(Work.Asset, Work.ProjectRoot, Work.Registry);
				}
				if (!Imported)
					throw std::runtime_error("HDR image import failed: " + Imported.error().Message);
				if (StopToken.stop_requested())
					return;

				const std::filesystem::path EnvironmentPath = GetEnvironmentCachePath(
					Work.ProjectRoot, Work.Asset, Imported->SourceFingerprint, Processing);
				std::optional<EnvironmentLightingData> Data;
				{
					Detail::ScopedProfileTimer Timer("Environment derived-data cache read");
					Data = ReadEnvironmentCache(EnvironmentPath, Work.Asset, Imported->SourceFingerprint, Processing);
				}
				const bool EnvironmentCacheHit = Data.has_value();
				if (!Data)
				{
					std::error_code RemoveError;
					std::filesystem::remove(EnvironmentPath, RemoveError);
				}

				const std::filesystem::path BrdfPath = GetBrdfCachePath(
					Work.ProjectRoot, Processing.BrdfLutSize, Processing.BrdfSamples);
				std::optional<ImportedFloatImage> BrdfLut;
				{
					Detail::ScopedProfileTimer Timer("Shared BRDF LUT cache read");
					BrdfLut = ReadBrdfCache(BrdfPath, Processing.BrdfLutSize, Processing.BrdfSamples);
				}
				if (!BrdfLut)
				{
					std::error_code RemoveError;
					std::filesystem::remove(BrdfPath, RemoveError);
					auto GeneratedBrdf = GenerateBrdfIntegrationLut(
						Processing.BrdfLutSize, Processing.BrdfSamples, StopToken);
					if (!GeneratedBrdf)
					{
						if (GeneratedBrdf.error().Code == EnvironmentProcessingErrorCode::Cancelled)
							return;
						throw std::runtime_error(GeneratedBrdf.error().Message);
					}
					BrdfLut = std::move(*GeneratedBrdf);
					WriteBrdfCache(BrdfPath, *BrdfLut, Processing.BrdfSamples);
				}

				if (!Data)
				{
					Detail::ScopedProfileTimer Timer("HDR CPU environment preprocessing");
					auto Processed = ProcessEnvironmentImage(*Imported, Processing, StopToken, &*BrdfLut);
					if (!Processed)
					{
						if (Processed.error().Code == EnvironmentProcessingErrorCode::Cancelled)
							return;
						throw std::runtime_error("Environment preprocessing failed: " + Processed.error().Message);
					}
					Data = std::move(*Processed);
					WriteEnvironmentCache(EnvironmentPath, Work.Asset, Imported->SourceFingerprint, Processing, *Data);
				}
				Data->BrdfIntegrationLut = std::move(*BrdfLut);
				PF_INFO("Environment derived data {} for {}", EnvironmentCacheHit ? "loaded from cache" : "generated",
					Work.Asset.ToString());
				{
					std::lock_guard EntryLock(Work.Destination->Mutex);
					Work.Destination->CpuData = std::move(*Data);
					Work.Destination->State = EntryState::CpuReady;
				}
			}
			catch (const std::exception& Exception)
			{
				std::lock_guard EntryLock(Work.Destination->Mutex);
				Work.Destination->Error = Exception.what();
				Work.Destination->State = EntryState::Failed;
				PF_ERROR("Background environment preparation failed for {}: {}", Work.Asset.ToString(), Exception.what());
			}
			catch (...)
			{
				std::lock_guard EntryLock(Work.Destination->Mutex);
				Work.Destination->Error = "Unknown background environment preparation failure";
				Work.Destination->State = EntryState::Failed;
				PF_ERROR("Background environment preparation failed for {} with an unknown error", Work.Asset.ToString());
			}
		}
	}

	std::expected<EnvironmentLightingTextures, EnvironmentLightingCacheError> EnvironmentLightingCache::UploadProcessedData(
		const AssetID& Asset,
		EnvironmentLightingData Data)
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
