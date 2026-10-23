#pragma once

#include "Assets/AssetRegistry.h"
#include "Renderer/RenderDevice.h"
#include "Renderer/Texture.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>

namespace PulseForge
{
	class DiagnosticRecorder;

	enum class TextureAssetErrorCode : uint8_t
	{
		AssetNotFound,
		UnsupportedFormat,
		ImportFailed,
		TextureCreationFailed,
		CacheFailure
	};

	struct TextureAssetError
	{
		TextureAssetErrorCode Code;
		AssetID Asset;
		std::string Message;
	};

	// Per-project runtime GPU cache. The rendering device, project registry and optional diagnostics recorder must outlive this cache.
	// Returned texture references remain valid until Clear() or cache destruction. The cache is not thread-safe.
	class TextureAssetCache final
	{
	public:
		TextureAssetCache(
			RenderDevice& Runtime,
			std::filesystem::path ProjectRoot,
			const AssetRegistry& Registry,
			DiagnosticRecorder* Diagnostics = nullptr);
		~TextureAssetCache();
		TextureAssetCache(const TextureAssetCache&) = delete;
		TextureAssetCache& operator=(const TextureAssetCache&) = delete;
		TextureAssetCache(TextureAssetCache&&) = delete;
		TextureAssetCache& operator=(TextureAssetCache&&) = delete;

		[[nodiscard]] std::expected<std::reference_wrapper<const Texture>, TextureAssetError> GetOrLoad(
			const AssetID& Asset, TextureFormat Format = TextureFormat::RGBA8_Srgb);
		void Clear() noexcept;
		[[nodiscard]] size_t GetLoadedCount() const noexcept;

	private:
		struct CacheKey
		{
			AssetID Asset;
			TextureFormat Format;

			friend bool operator==(const CacheKey&, const CacheKey&) = default;
		};

		struct CacheKeyHash
		{
			[[nodiscard]] size_t operator()(const CacheKey& Key) const noexcept;
		};

		RenderDevice& m_Runtime;
		std::filesystem::path m_ProjectRoot;
		const AssetRegistry& m_Registry;
		DiagnosticRecorder* m_Diagnostics = nullptr;
		uint64_t m_CacheHits = 0;
		uint64_t m_CacheMisses = 0;
		std::unordered_map<CacheKey, TextureHandle, CacheKeyHash> m_Textures;
	};
} // namespace PulseForge
