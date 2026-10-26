#pragma once

#include "Assets/AssetPreparationService.h"
#include "Assets/AssetRegistry.h"
#include "Renderer/AssetCache/AssetUploadBudget.h"
#include "Renderer/Mesh.h"
#include "Renderer/RenderDevice.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace PulseForge
{
	class DerivedDataArtifactStore;
	class DiagnosticRecorder;
	class CookedContentProvider;

	enum class MeshAssetErrorCode : uint8_t
	{
		AssetNotFound,
		Pending,
		ImportFailed,
		MeshCreationFailed,
		CacheFailure
	};

	struct MeshAssetError
	{
		MeshAssetErrorCode Code;
		AssetID Asset;
		std::string Message;
	};

	// Per-project runtime GPU cache. The rendering device, project registry, optional upload budget and diagnostics recorder
	// must outlive this cache.
	// Returned mesh references remain valid until a later AdvancePending() publishes a replacement, Clear(), or destruction.
	// The cache is not thread-safe.
	class MeshAssetCache final
	{
	public:
		MeshAssetCache(
			RenderDevice& Runtime,
			std::filesystem::path ProjectRoot,
			const AssetRegistry& Registry,
			DiagnosticRecorder* Diagnostics = nullptr);
		MeshAssetCache(RenderDevice& Runtime, std::shared_ptr<const CookedContentProvider> Content,
			DiagnosticRecorder* Diagnostics = nullptr);
		MeshAssetCache(
			RenderDevice& Runtime,
			std::filesystem::path ProjectRoot,
			const AssetRegistry& Registry,
			AssetPreparationService& Preparation,
			AssetUploadBudget* UploadBudget,
			DerivedDataArtifactStore& ArtifactStore,
			std::string SessionKey,
			DiagnosticRecorder* Diagnostics = nullptr);
		MeshAssetCache(
			RenderDevice& Runtime,
			std::filesystem::path ProjectRoot,
			std::shared_ptr<const AssetRegistry> Registry,
			AssetPreparationService& Preparation,
			AssetUploadBudget* UploadBudget,
			DerivedDataArtifactStore& ArtifactStore,
			std::string SessionKey,
			DiagnosticRecorder* Diagnostics = nullptr);
		~MeshAssetCache();
		MeshAssetCache(const MeshAssetCache&) = delete;
		MeshAssetCache& operator=(const MeshAssetCache&) = delete;
		MeshAssetCache(MeshAssetCache&&) = delete;
		MeshAssetCache& operator=(MeshAssetCache&&) = delete;

		// In asynchronous mode this admits preparation and returns the last published mesh. GPU publication happens only
		// in AdvancePending(), before a renderer begins resolving resources for a prepared frame.
		[[nodiscard]] std::expected<std::reference_wrapper<const Mesh>, MeshAssetError> GetOrLoad(const AssetID& Asset);
		[[nodiscard]] bool AdvancePending();
		// Change this on project close/reopen or a known source/settings refresh. Current GPU handles remain usable
		// while requests for the new session are prepared; work from the previous session is cancelled.
		void SetSessionKey(std::string SessionKey);
		void SetRegistrySnapshot(std::shared_ptr<const AssetRegistry> Registry) noexcept;
		[[nodiscard]] bool HasPendingWork() const noexcept;
		void Clear() noexcept;
		[[nodiscard]] size_t GetLoadedCount() const noexcept;

	private:
		[[nodiscard]] std::expected<std::reference_wrapper<const Mesh>, MeshAssetError> GetOrLoadInternal(
			const AssetID& Asset, bool CollectReadyProduct);

		struct Entry
		{
			MeshHandle Current;
			std::optional<AssetPreparationService::Subscription> Pending;
			std::string SessionKey;
			std::string FailureMessage;
			uint64_t DiagnosticSessionId = 0;
			bool DiagnosticTimingsRecorded = false;
			bool TerminalFailure = false;
			bool AdmissionDeferred = false;
			MeshAssetErrorCode TerminalFailureCode = MeshAssetErrorCode::MeshCreationFailed;
		};

		RenderDevice& m_Runtime;
		std::filesystem::path m_ProjectRoot;
		std::shared_ptr<const AssetRegistry> m_RegistryOwner;
		const AssetRegistry* m_Registry = nullptr;
		std::shared_ptr<const CookedContentProvider> m_CookedContent;
		AssetPreparationService* m_Preparation = nullptr;
		AssetUploadBudget* m_UploadBudget = nullptr;
		DerivedDataArtifactStore* m_ArtifactStore = nullptr;
		std::string m_SessionKey;
		DiagnosticRecorder* m_Diagnostics = nullptr;
		uint64_t m_CacheHits = 0;
		uint64_t m_CacheMisses = 0;
		std::unordered_map<AssetID, Entry, UUIDHash> m_Meshes;
	};
} // namespace PulseForge
