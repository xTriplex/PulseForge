#pragma once

#include "Assets/AssetRegistry.h"
#include "Assets/AssetPreparationService.h"
#include "Assets/DerivedDataArtifactStore.h"
#include "Renderer/EnvironmentLighting.h"
#include "Renderer/AssetCache/AssetUploadBudget.h"
#include "Renderer/RenderDevice.h"
#include "Renderer/EnvironmentLightingTextures.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace PulseForge
{
	class DiagnosticRecorder;

	namespace Detail
	{
		enum class EnvironmentCpuProductSource : uint8_t
		{
			Processed,
			LegacyArtifact,
			DerivedArtifact
		};

		// Owned CPU result for the production preparation path. Phase flags distinguish a disk artifact hit from
		// actual environment preprocessing; they are not claims about GPU residency or consumer publication.
		struct EnvironmentCpuPreparationResult
		{
			EnvironmentLightingData Data;
			EnvironmentCpuProductSource Source = EnvironmentCpuProductSource::Processed;
			bool EnvironmentArtifactHit = false;
			bool BrdfArtifactHit = false;
			bool RanEnvironmentPreprocessing = false;
			bool RanBrdfPreprocessing = false;
			uint64_t ImportStartTimestampNanoseconds = 0;
			uint64_t ImportEndTimestampNanoseconds = 0;
			uint64_t DerivedCacheReadStartTimestampNanoseconds = 0;
			uint64_t DerivedCacheReadEndTimestampNanoseconds = 0;
			uint64_t PreprocessStartTimestampNanoseconds = 0;
			uint64_t PreprocessEndTimestampNanoseconds = 0;
		};

		[[nodiscard]] PULSEFORGE_API std::expected<EnvironmentCpuPreparationResult, AssetPreparationError>
		PrepareEnvironmentCpuProduct(
			AssetID Asset,
			std::filesystem::path ProjectRoot,
			AssetRegistry Registry,
			DerivedDataArtifactStore* ArtifactStore,
			EnvironmentProcessingDesc Processing,
			std::stop_token StopToken = {});

		class EnvironmentLightingArtifactCodec final
		{
		public:
			[[nodiscard]] static DerivedDataArtifactKey MakeEnvironmentKey(
				const AssetID& Asset,
				const AssetContentFingerprint& SourceContent,
				const EnvironmentProcessingDesc& Description);
			[[nodiscard]] static DerivedDataArtifactKey MakeBrdfLutKey(const EnvironmentProcessingDesc& Description);
			[[nodiscard]] static std::expected<std::vector<std::byte>, std::string> EncodeEnvironment(
				const EnvironmentLightingData& Data,
				const EnvironmentProcessingDesc& Description);
			[[nodiscard]] static std::expected<EnvironmentLightingData, std::string> DecodeEnvironment(
				std::span<const std::byte> Payload,
				const EnvironmentProcessingDesc& Description);
			[[nodiscard]] static std::expected<std::vector<std::byte>, std::string> EncodeBrdfLut(
				const ImportedFloatImage& Image,
				uint32_t Samples);
			[[nodiscard]] static std::expected<ImportedFloatImage, std::string> DecodeBrdfLut(
				std::span<const std::byte> Payload,
				uint32_t ExpectedSize,
				uint32_t ExpectedSamples);
		};
	}

	struct EnvironmentLightingCacheError
	{
		AssetID Asset;
		std::string Message;
	};

	// CPU decoding/preprocessing is asynchronous; GPU upload and texture ownership stay on the renderer thread.
	// The optional diagnostics recorder must outlive this cache and is only used from synchronous renderer-thread calls.
	class EnvironmentLightingCache final
	{
	public:
		EnvironmentLightingCache(
			RenderDevice& Runtime,
			std::filesystem::path ProjectRoot,
			const AssetRegistry& Registry,
			DiagnosticRecorder* Diagnostics = nullptr,
			DerivedDataArtifactStoreOptions ArtifactStoreOptions = {});
		EnvironmentLightingCache(
			RenderDevice& Runtime,
			std::filesystem::path ProjectRoot,
			const AssetRegistry& Registry,
			DerivedDataArtifactStore& ArtifactStore,
			DiagnosticRecorder* Diagnostics = nullptr);
		EnvironmentLightingCache(
			RenderDevice& Runtime,
			std::filesystem::path ProjectRoot,
			const AssetRegistry& Registry,
			AssetPreparationService& Preparation,
			AssetUploadBudget* UploadBudget,
			DerivedDataArtifactStore& ArtifactStore,
			std::string SessionKey,
			DiagnosticRecorder* Diagnostics = nullptr,
			EnvironmentProcessingDesc Processing = {});
		~EnvironmentLightingCache();
		EnvironmentLightingCache(const EnvironmentLightingCache&) = delete;
		EnvironmentLightingCache& operator=(const EnvironmentLightingCache&) = delete;
		[[nodiscard]] std::expected<std::optional<std::reference_wrapper<const EnvironmentLightingTextures>>, EnvironmentLightingCacheError>
		GetOrLoad(const AssetID& Asset);
		// Call on a known project/source-session change. Pending subscriptions are cancelled; current GPU data remains usable
		// until a replacement has completed preparation and upload on the renderer owner.
		void SetSessionKey(std::string SessionKey);
		[[nodiscard]] size_t GetLoadedCount() const noexcept { return m_Entries.size(); }
		void Clear() noexcept;

	private:
		struct Entry
		{
			std::optional<AssetPreparationService::Subscription> Pending;
			std::optional<EnvironmentLightingTextures> GpuTextures;
			std::string Error;
			std::string SessionKey;
			std::string PublishedSessionKey;
			bool UploadFailed = false;
			uint64_t DiagnosticSessionId = 0;
		};

		[[nodiscard]] std::expected<EnvironmentLightingTextures, EnvironmentLightingCacheError> UploadProcessedData(
			const AssetID& Asset, const EnvironmentLightingData& Data);

		RenderDevice& m_Runtime;
		std::filesystem::path m_ProjectRoot;
		const AssetRegistry& m_Registry;
		DiagnosticRecorder* m_Diagnostics = nullptr;
		std::unique_ptr<AssetPreparationService> m_OwnedPreparationService;
		AssetPreparationService* m_Preparation = nullptr;
		AssetUploadBudget* m_UploadBudget = nullptr;
		std::unique_ptr<DerivedDataArtifactStore> m_OwnedArtifactStore;
		DerivedDataArtifactStore* m_ArtifactStore = nullptr;
		std::string m_SessionKey;
		EnvironmentProcessingDesc m_Processing;
		uint64_t m_CacheHits = 0;
		uint64_t m_CacheMisses = 0;
		std::unordered_map<AssetID, std::shared_ptr<Entry>, UUIDHash> m_Entries;
	};
} // namespace PulseForge
