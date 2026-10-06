#include "Core/PulseForgePCH.h"
#include "Assets/TextureAssetCache.h"

#include "Assets/ImageAssetImporter.h"
#include "Core/Application.h"

#include <exception>
#include <functional>
#include <utility>

namespace PulseForge
{
	TextureAssetCache::TextureAssetCache(
		Application& Runtime,
		std::filesystem::path ProjectRoot,
		const AssetRegistry& Registry)
		: m_Runtime(Runtime), m_ProjectRoot(std::move(ProjectRoot)), m_Registry(Registry)
	{
	}

	TextureAssetCache::~TextureAssetCache() = default;

	std::expected<std::reference_wrapper<const Texture>, TextureAssetError> TextureAssetCache::GetOrLoad(
		const AssetID& Asset,
		TextureFormat Format)
	{
		if (Asset.IsNil() || !m_Registry.Find(Asset))
		{
			return std::unexpected(TextureAssetError{
				TextureAssetErrorCode::AssetNotFound,
				Asset,
				Asset.IsNil()
					? "Cannot load a texture with a nil asset UUID"
					: "Texture asset UUID " + Asset.ToString() + " is not present in the project asset registry" });
		}

		if (Format != TextureFormat::RGBA8_UNorm && Format != TextureFormat::RGBA8_Srgb)
		{
			return std::unexpected(TextureAssetError{
				TextureAssetErrorCode::UnsupportedFormat,
				Asset,
				"Image assets can only be loaded as RGBA8_UNorm or RGBA8_Srgb textures" });
		}

		const CacheKey Key{ Asset, Format };
		if (const auto Existing = m_Textures.find(Key); Existing != m_Textures.end())
			return std::cref(*Existing->second);

		auto ImportedImage = ImageAssetImporter::ImportRGBA8(Asset, m_ProjectRoot, m_Registry);
		if (!ImportedImage)
		{
			return std::unexpected(TextureAssetError{
				TextureAssetErrorCode::ImportFailed,
				Asset,
				ImportedImage.error().Message });
		}

		TextureDesc Description;
		Description.Width = ImportedImage->Width;
		Description.Height = ImportedImage->Height;
		Description.Format = Format;
		Description.DebugName = "Image asset " + Asset.ToString();
		auto CreatedTexture = m_Runtime.CreateTexture(Description, std::as_bytes(std::span(ImportedImage->RGBA8Pixels)));
		if (!CreatedTexture)
		{
			return std::unexpected(TextureAssetError{
				TextureAssetErrorCode::TextureCreationFailed,
				Asset,
				CreatedTexture.error().Message });
		}

		try
		{
			auto [Inserted, WasInserted] = m_Textures.try_emplace(Key, std::move(CreatedTexture.value()));
			if (!WasInserted)
			{
				return std::unexpected(TextureAssetError{
					TextureAssetErrorCode::CacheFailure,
					Asset,
					"Texture asset UUID and format were already present during cache insertion" });
			}
			return std::cref(*Inserted->second);
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(TextureAssetError{
				TextureAssetErrorCode::CacheFailure,
				Asset,
				std::string("Could not retain the uploaded texture resource: ") + Exception.what() });
		}
	}

	void TextureAssetCache::Clear() noexcept
	{
		m_Textures.clear();
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
}
