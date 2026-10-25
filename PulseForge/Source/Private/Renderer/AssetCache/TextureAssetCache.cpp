#include "Renderer/AssetCache/TextureAssetCache.h"

#include "Assets/ImageAssetImporter.h"
#include "Assets/AssetPathResolver.h"
#include "Assets/DerivedDataArtifactStore.h"
#include "Core/Diagnostics.h"
#include "Assets/Detail/AssetCpuProducts.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <fstream>
#include <cmath>
#include <limits>
#include <span>
#include <string>
#include <system_error>
#include <vector>

namespace PulseForge
{
	namespace
	{
		using OwnedMipLevel = Detail::PreparedMipLevel;
		struct PreparedTextureData
		{
			uint32_t Width = 0;
			uint32_t Height = 0;
			TextureFormat Format = TextureFormat::RGBA8_Srgb;
			std::vector<OwnedMipLevel> MipLevels;
		};

		// Peak includes bounded source bytes, decoder output, retained mip levels, and an encoded cache-write buffer.
		constexpr size_t EstimatedTextureWorkingBytes = 480ull * 1024ull * 1024ull;
		constexpr uint32_t TextureArtifactSchemaVersion = 1;
		constexpr uint64_t MaximumImageSourceBytes = 128ull * 1024ull * 1024ull;
		constexpr uint64_t MaximumImagePixels = 32ull * 1024ull * 1024ull;
		constexpr size_t MaximumTextureArtifactBytes = 256ull * 1024ull * 1024ull;

		uint32_t GetFullMipCount(uint32_t Width, uint32_t Height)
		{
			uint32_t Count = 1;
			while (Width > 1 || Height > 1)
			{
				Width = (std::max)(1u, Width / 2);
				Height = (std::max)(1u, Height / 2);
				++Count;
			}
			return Count;
		}

		template <typename Product>
		size_t GetPreparedTextureRetainedBytes(const Product& Data) noexcept
		{
			size_t Total = 0;
			for (const OwnedMipLevel& Level : Data.MipLevels)
			{
				if (Level.Pixels.size() > (std::numeric_limits<size_t>::max)() - Total)
					return (std::numeric_limits<size_t>::max)();
				Total += Level.Pixels.size();
			}
			return Total;
		}

		void AppendU32(std::vector<std::byte>& Output, uint32_t Value)
		{
			for (unsigned Shift = 0; Shift < 32; Shift += 8)
				Output.push_back(static_cast<std::byte>((Value >> Shift) & 0xffu));
		}

		bool ReadU32(std::span<const std::byte> Input, size_t& Offset, uint32_t& Value)
		{
			if (Offset > Input.size() || Input.size() - Offset < 4) return false;
			Value = 0;
			for (unsigned Index = 0; Index < 4; ++Index)
				Value |= static_cast<uint32_t>(std::to_integer<uint8_t>(Input[Offset++])) << (Index * 8);
			return true;
		}

		std::expected<std::vector<std::byte>, std::string> ReadImageSource(const std::filesystem::path& Path)
		{
			std::error_code SizeError;
			const uintmax_t Size = std::filesystem::file_size(Path, SizeError);
			if (SizeError || Size == 0 || Size > MaximumImageSourceBytes || Size > std::numeric_limits<size_t>::max() ||
				Size > static_cast<uintmax_t>(std::numeric_limits<std::streamsize>::max()))
				return std::unexpected("Image source is inaccessible, empty or exceeds the bounded source limit");
			std::vector<std::byte> Bytes(static_cast<size_t>(Size));
			std::ifstream Input(Path, std::ios::binary);
			if (!Input.is_open()) return std::unexpected("Image source could not be opened for recipe identity");
			Input.read(reinterpret_cast<char*>(Bytes.data()), static_cast<std::streamsize>(Bytes.size()));
			if (!Input || Input.bad()) return std::unexpected("Image source could not be read completely for recipe identity");
			return Bytes;
		}

		std::array<uint64_t, 2> ComputeImporterImageFingerprint(std::span<const std::byte> Bytes)
		{
			uint64_t A = 14695981039346656037ull;
			uint64_t B = 7809847782465536322ull;
			for (const std::byte Byte : Bytes)
			{
				const uint8_t Value = std::to_integer<uint8_t>(Byte);
				A = (A ^ Value) * 1099511628211ull;
				B = (B ^ (Value + 0x9du)) * 14029467366897019727ull;
				B ^= B >> 29;
			}
			return { A, B };
		}

		DerivedDataArtifactKey MakeTextureArtifactKey(
			const AssetContentFingerprint& Source,
			TextureFormat Format)
		{
			AssetFingerprintBuilder Builder("PulseForge.TextureMipProduct.v1");
			Builder.UpdateText("sourceAlgorithm", Source.Algorithm);
			Builder.UpdateText("sourceDigest", Source.DigestHex);
			Builder.UpdateText("decoder", "PulseForge.ImageRGBA8Importer.v1");
			Builder.UpdateText("mipAlgorithm", "box-filter-srgb-linear-v1");
			Builder.UpdateText("format", std::to_string(static_cast<uint8_t>(Format)));
			return { "image-rgba8-mips", Builder.Finish(), TextureArtifactSchemaVersion };
		}

		std::expected<std::vector<std::byte>, std::string> EncodeTextureArtifact(const PreparedTextureData& Data)
		{
			if (Data.Width == 0 || Data.Height == 0 || Data.Width > 32768 || Data.Height > 32768 ||
				(static_cast<uint64_t>(Data.Width) * Data.Height) > MaximumImagePixels ||
				(Data.Format != TextureFormat::RGBA8_UNorm && Data.Format != TextureFormat::RGBA8_Srgb) ||
				Data.MipLevels.size() != GetFullMipCount(Data.Width, Data.Height))
				return std::unexpected("Texture product exceeds the artifact codec bounds");
			const size_t RetainedBytes = GetPreparedTextureRetainedBytes(Data);
			if (RetainedBytes > MaximumTextureArtifactBytes - 256)
				return std::unexpected("Texture product exceeds the artifact codec size bound");
			std::vector<std::byte> Output;
			Output.reserve(24 + RetainedBytes + Data.MipLevels.size() * 12);
			Output.insert(Output.end(), { std::byte{'P'}, std::byte{'F'}, std::byte{'T'}, std::byte{'X'} });
			AppendU32(Output, TextureArtifactSchemaVersion);
			AppendU32(Output, static_cast<uint32_t>(Data.Format));
			AppendU32(Output, Data.Width);
			AppendU32(Output, Data.Height);
			AppendU32(Output, static_cast<uint32_t>(Data.MipLevels.size()));
			uint32_t ExpectedWidth = Data.Width, ExpectedHeight = Data.Height;
			for (const OwnedMipLevel& Level : Data.MipLevels)
			{
				const uint64_t ExpectedBytes = static_cast<uint64_t>(ExpectedWidth) * ExpectedHeight * 4;
				if (Level.Width != ExpectedWidth || Level.Height != ExpectedHeight || Level.Pixels.size() != ExpectedBytes)
					return std::unexpected("Texture product has an inconsistent mip layout");
				AppendU32(Output, Level.Width);
				AppendU32(Output, Level.Height);
				AppendU32(Output, static_cast<uint32_t>(Level.Pixels.size()));
				for (uint8_t Value : Level.Pixels) Output.push_back(static_cast<std::byte>(Value));
				ExpectedWidth = (std::max)(1u, ExpectedWidth / 2);
				ExpectedHeight = (std::max)(1u, ExpectedHeight / 2);
			}
			if (ExpectedWidth != 1 || ExpectedHeight != 1 || Output.size() > MaximumTextureArtifactBytes)
				return std::unexpected("Texture product has incomplete mips or exceeds the artifact size bound");
			return Output;
		}

		std::expected<PreparedTextureData, std::string> DecodeTextureArtifact(
			std::span<const std::byte> Input, TextureFormat ExpectedFormat)
		{
			if (Input.size() < 24 || Input.size() > MaximumTextureArtifactBytes || Input[0] != std::byte{'P'} ||
				Input[1] != std::byte{'F'} || Input[2] != std::byte{'T'} || Input[3] != std::byte{'X'})
				return std::unexpected("Texture artifact has an invalid magic or size");
			size_t Offset = 4;
			uint32_t Version = 0, Format = 0, MipCount = 0;
			PreparedTextureData Data;
			if (!ReadU32(Input, Offset, Version) || Version != TextureArtifactSchemaVersion ||
				!ReadU32(Input, Offset, Format) || Format != static_cast<uint32_t>(ExpectedFormat) ||
				!ReadU32(Input, Offset, Data.Width) || !ReadU32(Input, Offset, Data.Height) ||
				!ReadU32(Input, Offset, MipCount) || Data.Width == 0 || Data.Height == 0 ||
				Data.Width > 32768 || Data.Height > 32768 || static_cast<uint64_t>(Data.Width) * Data.Height > MaximumImagePixels ||
				MipCount != GetFullMipCount(Data.Width, Data.Height))
				return std::unexpected("Texture artifact has invalid schema, format or dimensions");
			Data.Format = ExpectedFormat;
			uint32_t ExpectedWidth = Data.Width, ExpectedHeight = Data.Height;
			Data.MipLevels.reserve(MipCount);
			for (uint32_t Index = 0; Index < MipCount; ++Index)
			{
				uint32_t Width = 0, Height = 0, ByteCount = 0;
				const uint64_t ExpectedBytes = static_cast<uint64_t>(ExpectedWidth) * ExpectedHeight * 4;
				if (!ReadU32(Input, Offset, Width) || !ReadU32(Input, Offset, Height) || !ReadU32(Input, Offset, ByteCount) ||
					Width != ExpectedWidth || Height != ExpectedHeight || ByteCount != ExpectedBytes ||
					Offset > Input.size() || Input.size() - Offset < ByteCount)
					return std::unexpected("Texture artifact contains an invalid mip layout or truncated payload");
				OwnedMipLevel Level{ Width, Height, {} };
				Level.Pixels.resize(ByteCount);
				for (uint8_t& Value : Level.Pixels) Value = std::to_integer<uint8_t>(Input[Offset++]);
				Data.MipLevels.push_back(std::move(Level));
				ExpectedWidth = (std::max)(1u, ExpectedWidth / 2);
				ExpectedHeight = (std::max)(1u, ExpectedHeight / 2);
			}
			if (ExpectedWidth != 1 || ExpectedHeight != 1 || Offset != Input.size())
				return std::unexpected("Texture artifact has trailing data or an incomplete mip chain");
			return Data;
		}

		float SrgbToLinear(uint8_t Value)
		{
			const float Encoded = static_cast<float>(Value) / 255.0f;
			return Encoded <= 0.04045f ? Encoded / 12.92f : std::pow((Encoded + 0.055f) / 1.055f, 2.4f);
		}

		uint8_t LinearToSrgbByte(float Value)
		{
			Value = std::clamp(Value, 0.0f, 1.0f);
			const float Encoded = Value <= 0.0031308f ? Value * 12.92f : 1.055f * std::pow(Value, 1.0f / 2.4f) - 0.055f;
			return static_cast<uint8_t>(std::lround(Encoded * 255.0f));
		}

		std::vector<OwnedMipLevel> BuildMipChain(uint32_t Width, uint32_t Height, std::span<const uint8_t> SourcePixels, bool IsSrgb)
		{
			std::vector<OwnedMipLevel> Levels;
			Levels.push_back({ Width, Height, { SourcePixels.begin(), SourcePixels.end() } });
			while (Width > 1 || Height > 1)
			{
				const OwnedMipLevel& Previous = Levels.back();
				const uint32_t NextWidth = (std::max)(1u, Width / 2);
				const uint32_t NextHeight = (std::max)(1u, Height / 2);
				OwnedMipLevel Next;
				Next.Width = NextWidth;
				Next.Height = NextHeight;
				Next.Pixels.resize(static_cast<size_t>(NextWidth) * NextHeight * 4);
				for (uint32_t Y = 0; Y < NextHeight; ++Y)
				{
					const uint32_t StartY = static_cast<uint32_t>((static_cast<uint64_t>(Y) * Height) / NextHeight);
					const uint32_t EndY = static_cast<uint32_t>((static_cast<uint64_t>(Y + 1) * Height) / NextHeight);
					for (uint32_t X = 0; X < NextWidth; ++X)
					{
						const uint32_t StartX = static_cast<uint32_t>((static_cast<uint64_t>(X) * Width) / NextWidth);
						const uint32_t EndX = static_cast<uint32_t>((static_cast<uint64_t>(X + 1) * Width) / NextWidth);
						std::array<float, 4> Sum{};
						uint32_t SampleCount = 0;
						for (uint32_t SourceY = StartY; SourceY < EndY; ++SourceY)
							for (uint32_t SourceX = StartX; SourceX < EndX; ++SourceX)
							{
								const size_t Source = (static_cast<size_t>(SourceY) * Width + SourceX) * 4;
								for (size_t Channel = 0; Channel < 4; ++Channel)
									Sum[Channel] += IsSrgb && Channel < 3 ? SrgbToLinear(Previous.Pixels[Source + Channel])
																		  : static_cast<float>(Previous.Pixels[Source + Channel]) / 255.0f;
								++SampleCount;
							}
						const size_t Destination = (static_cast<size_t>(Y) * NextWidth + X) * 4;
						for (size_t Channel = 0; Channel < 4; ++Channel)
						{
							const float Average = Sum[Channel] / static_cast<float>(SampleCount);
							Next.Pixels[Destination + Channel] =
								IsSrgb && Channel < 3 ? LinearToSrgbByte(Average) : static_cast<uint8_t>(std::lround(Average * 255.0f));
						}
					}
				}
				Levels.push_back(std::move(Next));
				Width = NextWidth;
				Height = NextHeight;
			}
			return Levels;
		}
	} // namespace

	TextureAssetCache::TextureAssetCache(
		RenderDevice& Runtime,
		std::filesystem::path ProjectRoot,
		const AssetRegistry& Registry,
		DiagnosticRecorder* Diagnostics)
		: m_Runtime(Runtime), m_ProjectRoot(std::move(ProjectRoot)), m_Registry(Registry), m_Diagnostics(Diagnostics)
	{
	}

	TextureAssetCache::TextureAssetCache(
		RenderDevice& Runtime,
		std::filesystem::path ProjectRoot,
		const AssetRegistry& Registry,
		AssetPreparationService& Preparation,
		AssetUploadBudget* UploadBudget,
		DerivedDataArtifactStore& ArtifactStore,
		std::string SessionKey,
		DiagnosticRecorder* Diagnostics)
		: m_Runtime(Runtime), m_ProjectRoot(std::move(ProjectRoot)), m_Registry(Registry),
		  m_Preparation(&Preparation), m_UploadBudget(UploadBudget), m_ArtifactStore(&ArtifactStore),
		  m_SessionKey(std::move(SessionKey)), m_Diagnostics(Diagnostics)
	{
	}

	TextureAssetCache::~TextureAssetCache()
	{
		Clear();
	}

	void TextureAssetCache::SetSessionKey(std::string SessionKey)
	{
		if (SessionKey == m_SessionKey)
			return;
		m_SessionKey = std::move(SessionKey);
		for (auto& [Key, Entry] : m_Textures)
		{
			(void)Key;
			Entry.Pending.reset();
			Entry.SessionKey.clear();
			Entry.FailureMessage.clear();
			Entry.UploadFailed = false;
		}
	}

	std::expected<std::reference_wrapper<const Texture>, TextureAssetError> TextureAssetCache::GetOrLoad(
		const AssetID& Asset, TextureFormat Format)
	{
		if (Asset.IsNil() || !m_Registry.Find(Asset))
		{
			return std::unexpected(TextureAssetError{ TextureAssetErrorCode::AssetNotFound, Asset,
				Asset.IsNil() ? "Cannot load a texture with a nil asset UUID"
							  : "Texture asset UUID " + Asset.ToString() + " is not present in the project asset registry" });
		}

		if (Format != TextureFormat::RGBA8_UNorm && Format != TextureFormat::RGBA8_Srgb)
		{
			return std::unexpected(TextureAssetError{
				TextureAssetErrorCode::UnsupportedFormat, Asset, "Image assets can only be loaded as RGBA8_UNorm or RGBA8_Srgb textures" });
		}

		const CacheKey Key{ Asset, Format };
		auto [EntryIterator, WasInserted] = m_Textures.try_emplace(Key);
		(void)WasInserted;
		Entry& State = EntryIterator->second;
		if (!m_Preparation)
		{
			if (State.Current)
			{
				++m_CacheHits;
				return std::cref(*State.Current);
			}
			++m_CacheMisses;
			auto ImportedImage = [&]()
			{
				ScopedDiagnosticTimer Timer(m_Diagnostics, "Texture asset import", "TextureAssetCache");
				return ImageAssetImporter::ImportRGBA8(Asset, m_ProjectRoot, m_Registry);
			}();
			if (!ImportedImage)
				return std::unexpected(TextureAssetError{ TextureAssetErrorCode::ImportFailed, Asset, ImportedImage.error().Message });
			PreparedTextureData Prepared;
			Prepared.Width = ImportedImage->Width;
			Prepared.Height = ImportedImage->Height;
			Prepared.Format = Format;
			{
				ScopedDiagnosticTimer Timer(m_Diagnostics, "Texture mip generation", "TextureAssetCache");
				Prepared.MipLevels = BuildMipChain(ImportedImage->Width, ImportedImage->Height, ImportedImage->RGBA8Pixels,
					Format == TextureFormat::RGBA8_Srgb);
			}
			TextureDesc Description;
			Description.Width = Prepared.Width;
			Description.Height = Prepared.Height;
			Description.Format = Prepared.Format;
			Description.MipLevels = static_cast<uint32_t>(Prepared.MipLevels.size());
			Description.DebugName = "Image asset " + Asset.ToString();
			std::vector<TextureSubresourceData> InitialData;
			InitialData.reserve(Prepared.MipLevels.size());
			for (uint32_t MipLevel = 0; MipLevel < Prepared.MipLevels.size(); ++MipLevel)
			{
				const OwnedMipLevel& Level = Prepared.MipLevels[MipLevel];
				InitialData.push_back({ MipLevel, 0, std::as_bytes(std::span(Level.Pixels)), 0 });
			}
			auto CreatedTexture = m_Runtime.CreateTexture(Description, InitialData);
			if (!CreatedTexture)
				return std::unexpected(TextureAssetError{ TextureAssetErrorCode::TextureCreationFailed, Asset,
					CreatedTexture.error().Message });
			State.Current = std::move(*CreatedTexture);
			if (m_Diagnostics)
				(void)m_Diagnostics->RecordEvent(DiagnosticEventKind::Resource, "Texture cache resource created", "TextureAssetCache", "objects", 1);
			return std::cref(*State.Current);
		}

		if (State.SessionKey != m_SessionKey)
		{
			if (m_SessionKey.empty())
			{
				if (State.Current)
					return std::cref(*State.Current);
				return std::unexpected(TextureAssetError{ TextureAssetErrorCode::CacheFailure, Asset,
					"Asynchronous texture loading requires a non-empty project/session identity" });
			}
			State.Pending.reset();
			State.FailureMessage.clear();
			State.DiagnosticSessionId = 0;
			State.DiagnosticTimingsRecorded = false;
			if (m_Diagnostics)
			{
				uint64_t Start = 0;
				(void)m_Diagnostics->BeginDurationSession(State.DiagnosticSessionId, Start);
			}
			State.UploadFailed = false;
			AssetRegistry RegistrySnapshot = m_Registry;
			const std::filesystem::path ProjectRoot = m_ProjectRoot;
			DerivedDataArtifactStore* ArtifactStore = m_ArtifactStore;
			auto Requested = m_Preparation->Request(
				{ m_SessionKey, "texture-request/" + Asset.ToString() + "/" + std::to_string(static_cast<uint8_t>(Format)) },
				AssetPreparationPriority::Interactive,
				EstimatedTextureWorkingBytes,
				[Asset, Format, ProjectRoot, RegistrySnapshot = std::move(RegistrySnapshot), ArtifactStore](std::stop_token StopToken) mutable
					-> std::expected<AssetPreparationService::CpuPayload, AssetPreparationError>
				{
					auto Prepared = Detail::PrepareTextureCpuProduct(
						Asset, ProjectRoot, RegistrySnapshot,
						Format == TextureFormat::RGBA8_Srgb ? Detail::ImageColorSpace::Srgb : Detail::ImageColorSpace::Linear,
						ArtifactStore, StopToken);
					if (!Prepared)
						return std::unexpected(AssetPreparationError{ AssetPreparationErrorCode::ProcessingFailed, Prepared.error() });
					const size_t RetainedBytes = GetPreparedTextureRetainedBytes(Prepared->Product);
					return MakeAssetPreparationValue(std::move(*Prepared), RetainedBytes);
				});
			if (!Requested)
			{
				if (State.Current)
					return std::cref(*State.Current);
				if (Requested.error().Code == AssetPreparationErrorCode::WorkingSetLimit ||
					Requested.error().Code == AssetPreparationErrorCode::QueueFull)
					return std::unexpected(TextureAssetError{ TextureAssetErrorCode::Pending, Asset, Requested.error().Message });
				return std::unexpected(TextureAssetError{ TextureAssetErrorCode::CacheFailure, Asset, Requested.error().Message });
			}
			State.Pending.emplace(std::move(*Requested));
			State.SessionKey = m_SessionKey;
			++m_CacheMisses;
		}

		if (!State.Pending)
		{
			if (State.Current)
				return std::cref(*State.Current);
			if (State.UploadFailed)
				return std::unexpected(TextureAssetError{ TextureAssetErrorCode::TextureCreationFailed, Asset, State.FailureMessage });
			return std::unexpected(TextureAssetError{ TextureAssetErrorCode::Pending, Asset, "Texture preparation is pending" });
		}

		const AssetPreparationSnapshot Prepared = State.Pending->Poll();
		switch (Prepared.Status)
		{
			case AssetPreparationStatus::Queued:
			case AssetPreparationStatus::Processing:
				if (State.Current)
					return std::cref(*State.Current);
				return std::unexpected(TextureAssetError{ TextureAssetErrorCode::Pending, Asset, "Texture preparation is pending" });
			case AssetPreparationStatus::CpuReady:
			{
				if (State.UploadFailed)
				{
					if (State.Current)
						return std::cref(*State.Current);
					return std::unexpected(TextureAssetError{ TextureAssetErrorCode::TextureCreationFailed, Asset,
						State.FailureMessage });
				}
				const auto* TextureProduct = Prepared.CpuPayload
					? dynamic_cast<const AssetPreparationValue<Detail::AssetCpuProductResult<Detail::PreparedTextureCpuProduct>>*>(Prepared.CpuPayload.get()) : nullptr;
				if (!TextureProduct)
				{
					State.UploadFailed = true;
					State.FailureMessage = "Prepared texture result has an unexpected CPU product type";
					if (State.Current)
						return std::cref(*State.Current);
					return std::unexpected(TextureAssetError{ TextureAssetErrorCode::CacheFailure, Asset, State.FailureMessage });
				}
				const auto& CpuResult = TextureProduct->Get();
				const auto& Data = CpuResult.Product;
				if (!State.DiagnosticTimingsRecorded && m_Diagnostics)
				{
					const auto RecordPhase = [this, &State](const Detail::AssetCpuPhaseTiming& Timing, std::string_view Name)
					{
						if (Timing.EndTimestampNanoseconds > Timing.StartTimestampNanoseconds)
							(void)m_Diagnostics->RecordDurationForSession(State.DiagnosticSessionId,
								Timing.StartTimestampNanoseconds, Timing.EndTimestampNanoseconds, Name, "TextureAssetCache");
					};
					RecordPhase(CpuResult.ObservationAndLookup, "Image source read and artifact lookup");
					RecordPhase(CpuResult.Processing, "Image decode and mip processing");
					RecordPhase(CpuResult.CacheWrite, "Image artifact write");
					(void)m_Diagnostics->RecordEvent(DiagnosticEventKind::Counter,
						CpuResult.CacheOutcome == Detail::AssetCpuProductCacheOutcome::ArtifactHit ? "Image artifact cache hit" : "Image artifact cache miss",
						"TextureAssetCache", "products", 1);
					State.DiagnosticTimingsRecorded = true;
				}
				const size_t UploadBytes = Prepared.CpuPayload->GetRetainedBytes();
				if (m_UploadBudget && (UploadBytes == 0 || UploadBytes > AssetUploadBudget::MaximumIndivisibleProductBytes))
				{
					State.UploadFailed = true;
					State.FailureMessage = "Prepared texture exceeds the bounded indivisible GPU upload size";
					State.Pending.reset();
					if (State.Current) return std::cref(*State.Current);
					return std::unexpected(TextureAssetError{ TextureAssetErrorCode::TextureCreationFailed, Asset, State.FailureMessage });
				}
				if (m_UploadBudget && !m_UploadBudget->TryReserve(UploadBytes))
				{
					if (State.Current) return std::cref(*State.Current);
					return std::unexpected(TextureAssetError{ TextureAssetErrorCode::Pending, Asset,
						"Texture CPU product is ready and waiting for the renderer upload budget" });
				}
				TextureDesc Description;
				Description.Width = Data.Width;
				Description.Height = Data.Height;
				Description.Format = Data.ColorSpace == Detail::ImageColorSpace::Srgb
					? TextureFormat::RGBA8_Srgb : TextureFormat::RGBA8_UNorm;
				Description.MipLevels = static_cast<uint32_t>(Data.MipLevels.size());
				Description.DebugName = "Image asset " + Asset.ToString();
				std::vector<TextureSubresourceData> InitialData;
				InitialData.reserve(Data.MipLevels.size());
				for (uint32_t MipLevel = 0; MipLevel < Data.MipLevels.size(); ++MipLevel)
				{
					const OwnedMipLevel& Level = Data.MipLevels[MipLevel];
					InitialData.push_back({ MipLevel, 0, std::as_bytes(std::span(Level.Pixels)), 0 });
				}
				auto CreatedTexture = [&]()
				{
					ScopedDiagnosticTimer Timer(m_Diagnostics, "Texture GPU resource creation", "TextureAssetCache");
					return m_Runtime.CreateTexture(Description, InitialData);
				}();
				if (!CreatedTexture)
				{
					State.UploadFailed = true;
					State.FailureMessage = CreatedTexture.error().Message;
					if (State.Current)
						return std::cref(*State.Current);
					return std::unexpected(TextureAssetError{ TextureAssetErrorCode::TextureCreationFailed, Asset,
						State.FailureMessage });
				}
				State.Current = std::move(*CreatedTexture);
				State.Pending.reset();
				State.UploadFailed = false;
				State.FailureMessage.clear();
				if (m_Diagnostics)
					(void)m_Diagnostics->RecordEvent(DiagnosticEventKind::Resource, "Texture cache resource created", "TextureAssetCache", "objects", 1);
				return std::cref(*State.Current);
			}
			case AssetPreparationStatus::Failed:
				State.FailureMessage = Prepared.Error ? Prepared.Error->Message : "Texture CPU preparation failed";
				if (State.Current)
					return std::cref(*State.Current);
				return std::unexpected(TextureAssetError{ TextureAssetErrorCode::ImportFailed, Asset, State.FailureMessage });
			case AssetPreparationStatus::Cancelled:
				State.Pending.reset();
				State.SessionKey.clear();
				if (State.Current)
					return std::cref(*State.Current);
				return std::unexpected(TextureAssetError{ TextureAssetErrorCode::Pending, Asset, "Texture preparation was cancelled" });
		}
		return std::unexpected(TextureAssetError{ TextureAssetErrorCode::CacheFailure, Asset,
			"Unrecognized texture preparation state" });
	}

	void TextureAssetCache::Clear() noexcept
	{
		const uint64_t RetiredCount = GetLoadedCount();
		m_Textures.clear();
		if (m_Diagnostics && RetiredCount > 0)
			(void)m_Diagnostics->RecordEvent(DiagnosticEventKind::Resource, "Texture cache resources retired", "TextureAssetCache", "objects", RetiredCount);
		if (m_Diagnostics && m_CacheHits > 0)
			(void)m_Diagnostics->RecordEvent(DiagnosticEventKind::Counter, "Texture cache hits", "TextureAssetCache", "lookups", m_CacheHits);
		if (m_Diagnostics && m_CacheMisses > 0)
			(void)m_Diagnostics->RecordEvent(DiagnosticEventKind::Counter, "Texture cache misses", "TextureAssetCache", "lookups", m_CacheMisses);
		m_CacheHits = 0;
		m_CacheMisses = 0;
	}

	size_t TextureAssetCache::GetLoadedCount() const noexcept
	{
		size_t Count = 0;
		for (const auto& [Key, Entry] : m_Textures)
		{
			(void)Key;
			Count += Entry.Current ? 1 : 0;
		}
		return Count;
	}

	size_t TextureAssetCache::CacheKeyHash::operator()(const CacheKey& Key) const noexcept
	{
		const size_t AssetHash = UUIDHash{}(Key.Asset);
		const size_t FormatHash = std::hash<uint8_t>{}(static_cast<uint8_t>(Key.Format));
		return AssetHash ^ (FormatHash + static_cast<size_t>(0x9e3779b9) + (AssetHash << 6) + (AssetHash >> 2));
	}
} // namespace PulseForge
