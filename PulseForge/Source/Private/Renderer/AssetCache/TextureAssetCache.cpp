#include "Renderer/AssetCache/TextureAssetCache.h"

#include "Assets/ImageAssetImporter.h"
#include "Core/Diagnostics.h"

#include <exception>
#include <cmath>

namespace PulseForge
{
	namespace
	{
		struct OwnedMipLevel
		{
			uint32_t Width = 0;
			uint32_t Height = 0;
			std::vector<uint8_t> Pixels;
		};

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

	TextureAssetCache::~TextureAssetCache()
	{
		Clear();
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
		if (const auto Existing = m_Textures.find(Key); Existing != m_Textures.end())
		{
			++m_CacheHits;
			return std::cref(*Existing->second);
		}
		++m_CacheMisses;

		auto ImportedImage = [&]()
		{
			ScopedDiagnosticTimer Timer(m_Diagnostics, "Texture asset import", "TextureAssetCache");
			return ImageAssetImporter::ImportRGBA8(Asset, m_ProjectRoot, m_Registry);
		}();
		if (!ImportedImage)
		{
			return std::unexpected(TextureAssetError{ TextureAssetErrorCode::ImportFailed, Asset, ImportedImage.error().Message });
		}

		TextureDesc Description;
		Description.Width = ImportedImage->Width;
		Description.Height = ImportedImage->Height;
		Description.Format = Format;
		auto MipLevels = [&]()
		{
			ScopedDiagnosticTimer Timer(m_Diagnostics, "Texture mip generation", "TextureAssetCache");
			return BuildMipChain(
				ImportedImage->Width, ImportedImage->Height, ImportedImage->RGBA8Pixels, Format == TextureFormat::RGBA8_Srgb);
		}();
		Description.MipLevels = static_cast<uint32_t>(MipLevels.size());
		Description.DebugName = "Image asset " + Asset.ToString();
		std::vector<TextureSubresourceData> InitialData;
		InitialData.reserve(MipLevels.size());
		for (uint32_t MipLevel = 0; MipLevel < MipLevels.size(); ++MipLevel)
		{
			const OwnedMipLevel& Level = MipLevels[MipLevel];
			InitialData.push_back({ MipLevel, 0, std::as_bytes(std::span(Level.Pixels)), 0 });
		}
		auto CreatedTexture = [&]()
		{
			ScopedDiagnosticTimer Timer(m_Diagnostics, "Texture GPU resource creation", "TextureAssetCache");
			return m_Runtime.CreateTexture(Description, InitialData);
		}();
		if (!CreatedTexture)
		{
			return std::unexpected(
				TextureAssetError{ TextureAssetErrorCode::TextureCreationFailed, Asset, CreatedTexture.error().Message });
		}

		try
		{
			auto [Inserted, WasInserted] = m_Textures.try_emplace(Key, std::move(CreatedTexture.value()));
			if (!WasInserted)
			{
				return std::unexpected(TextureAssetError{ TextureAssetErrorCode::CacheFailure, Asset,
					"Texture asset UUID and format were already present during cache insertion" });
			}
			if (m_Diagnostics)
				(void)m_Diagnostics->RecordEvent(DiagnosticEventKind::Resource, "Texture cache resource created", "TextureAssetCache", "objects", 1);
			return std::cref(*Inserted->second);
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(TextureAssetError{ TextureAssetErrorCode::CacheFailure, Asset,
				std::string("Could not retain the uploaded texture resource: ") + Exception.what() });
		}
	}

	void TextureAssetCache::Clear() noexcept
	{
		const uint64_t RetiredCount = m_Textures.size();
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
		return m_Textures.size();
	}

	size_t TextureAssetCache::CacheKeyHash::operator()(const CacheKey& Key) const noexcept
	{
		const size_t AssetHash = UUIDHash{}(Key.Asset);
		const size_t FormatHash = std::hash<uint8_t>{}(static_cast<uint8_t>(Key.Format));
		return AssetHash ^ (FormatHash + static_cast<size_t>(0x9e3779b9) + (AssetHash << 6) + (AssetHash >> 2));
	}
} // namespace PulseForge
