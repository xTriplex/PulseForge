#pragma once

#include "Assets/AssetPreparationService.h"
#include "Assets/AssetRegistry.h"
#include "Renderer/AssetCache/AssetUploadBudget.h"
#include "Renderer/RenderDevice.h"
#include "Renderer/Texture.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>

namespace PulseForge
{
	class DerivedDataArtifactStore;
	class DiagnosticRecorder;

	enum class TextureAssetErrorCode : uint8_t
	{
		AssetNotFound,
		Pending,
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

	// Per-project runtime GPU cache. The rendering device, project registry, optional upload budget and diagnostics recorder
	// must outlive this cache.
	// Returned texture references remain valid until Clear() or cache destruction. The cache is not thread-safe.
	class TextureAssetCache final
	{
	public:
		TextureAssetCache(
			RenderDevice& Runtime,
			std::filesystem::path ProjectRoot,
			const AssetRegistry& Registry,
			DiagnosticRecorder* Diagnostics = nullptr);
		TextureAssetCache(
			RenderDevice& Runtime,
			std::filesystem::path ProjectRoot,
			const AssetRegistry& Registry,
			AssetPreparationService& Preparation,
			AssetUploadBudget* UploadBudget,
			DerivedDataArtifactStore& ArtifactStore,
			std::string SessionKey,
			DiagnosticRecorder* Diagnostics = nullptr);
		~TextureAssetCache();
		TextureAssetCache(const TextureAssetCache&) = delete;
		TextureAssetCache& operator=(const TextureAssetCache&) = delete;
		TextureAssetCache(TextureAssetCache&&) = delete;
		TextureAssetCache& operator=(TextureAssetCache&&) = delete;

		[[nodiscard]] std::expected<std::reference_wrapper<const Texture>, TextureAssetError> GetOrLoad(
			const AssetID& Asset, TextureFormat Format = TextureFormat::RGBA8_Srgb);
		// Change this on project close/reopen or a known source/settings refresh. Current GPU handles remain usable
		// while requests for the new session are prepared; work from the previous session is cancelled.
		void SetSessionKey(std::string SessionKey);
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
		struct Entry
		{
			TextureHandle Current;
			std::optional<AssetPreparationService::Subscription> Pending;
			std::string SessionKey;
			std::string FailureMessage;
			uint64_t DiagnosticSessionId = 0;
			bool DiagnosticTimingsRecorded = false;
			bool UploadFailed = false;
		};

		RenderDevice& m_Runtime;
		std::filesystem::path m_ProjectRoot;
		const AssetRegistry& m_Registry;
		AssetPreparationService* m_Preparation = nullptr;
		AssetUploadBudget* m_UploadBudget = nullptr;
		DerivedDataArtifactStore* m_ArtifactStore = nullptr;
		std::string m_SessionKey;
		DiagnosticRecorder* m_Diagnostics = nullptr;
		uint64_t m_CacheHits = 0;
		uint64_t m_CacheMisses = 0;
		std::unordered_map<CacheKey, Entry, CacheKeyHash> m_Textures;
	};
} // namespace PulseForge
