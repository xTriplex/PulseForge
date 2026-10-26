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
	class CookedContentProvider;

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
			const AssetRegistry& Registry,
			DerivedDataArtifactStore* ArtifactStore,
			EnvironmentProcessingDesc Processing,
			std::stop_token StopToken = {});

	}

	struct EnvironmentLightingCacheError
	{
		AssetID Asset;
		std::string Message;
	};

	// CPU decoding/preprocessing is asynchronous; GPU upload and texture ownership stay on the renderer thread.
	// Returned resource references remain valid until a later AdvancePending() publishes a replacement, Clear(), or destruction.
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
		EnvironmentLightingCache(RenderDevice& Runtime, std::shared_ptr<const CookedContentProvider> Content,
			DiagnosticRecorder* Diagnostics = nullptr);
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
		EnvironmentLightingCache(
			RenderDevice& Runtime,
			std::filesystem::path ProjectRoot,
			std::shared_ptr<const AssetRegistry> Registry,
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
		// Publishes completed renderer resources as one pre-frame phase. GetOrLoad only admits/resolves in async mode.
		// Advance admitted CPU products and renderer uploads independently of binding-cache lookup results.
		[[nodiscard]] bool AdvancePending();
		[[nodiscard]] uint64_t GetResourceRevision(const AssetID& Asset) const noexcept;
		[[nodiscard]] bool HasPendingWork() const noexcept;
		// Call on a known project/source-session change. Pending subscriptions are cancelled; current GPU data remains usable
		// until a replacement has completed preparation and upload on the renderer owner.
		void SetSessionKey(std::string SessionKey);
		void SetRegistrySnapshot(std::shared_ptr<const AssetRegistry> Registry) noexcept;
		[[nodiscard]] size_t GetLoadedCount() const noexcept { return m_Entries.size(); }
		void Clear() noexcept;

	private:
		[[nodiscard]] std::expected<std::optional<std::reference_wrapper<const EnvironmentLightingTextures>>, EnvironmentLightingCacheError>
		GetOrLoadInternal(const AssetID& Asset, bool CollectReadyProduct);

		struct Entry
		{
			std::optional<AssetPreparationService::Subscription> Pending;
			std::optional<EnvironmentLightingTextures> GpuTextures;
			std::string Error;
			std::string SessionKey;
			std::string PublishedSessionKey;
			bool UploadFailed = false;
			bool AdmissionDeferred = false;
			uint64_t DiagnosticSessionId = 0;
			uint64_t ResourceRevision = 0;
		};

		[[nodiscard]] std::expected<EnvironmentLightingTextures, EnvironmentLightingCacheError> UploadProcessedData(
			const AssetID& Asset, const EnvironmentLightingData& Data);

		RenderDevice& m_Runtime;
#if !defined(PF_COOKED_RUNTIME_ONLY)
		std::filesystem::path m_ProjectRoot;
		std::shared_ptr<const AssetRegistry> m_RegistryOwner;
		const AssetRegistry* m_Registry = nullptr;
#endif
		std::shared_ptr<const CookedContentProvider> m_CookedContent;
		DiagnosticRecorder* m_Diagnostics = nullptr;
#if !defined(PF_COOKED_RUNTIME_ONLY)
		std::unique_ptr<AssetPreparationService> m_OwnedPreparationService;
		AssetPreparationService* m_Preparation = nullptr;
		AssetUploadBudget* m_UploadBudget = nullptr;
		std::unique_ptr<DerivedDataArtifactStore> m_OwnedArtifactStore;
		DerivedDataArtifactStore* m_ArtifactStore = nullptr;
		std::string m_SessionKey;
		EnvironmentProcessingDesc m_Processing;
#endif
		uint64_t m_CacheHits = 0;
		uint64_t m_CacheMisses = 0;
		std::unordered_map<AssetID, std::shared_ptr<Entry>, UUIDHash> m_Entries;
	};
} // namespace PulseForge
