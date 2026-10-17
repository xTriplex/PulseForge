#pragma once

#include "Renderer/Texture.h"
#include "Editor/EditorAssetPreviewPolicy.h"
#include "Editor/EditorTextureRegistry.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace PulseForge
{
	class RenderDevice;
}

namespace PulseForgeEditor
{
	struct AssetPreviewServiceLimits
	{
		// Zero queue/entry limits reject new requests. Zero CPU/GPU limits make every non-empty result fail.
		// Zero upload count or bytes disables preview uploads and marks decoded requests failed.
		size_t MaximumQueuedRequests = 96;
		size_t MaximumEntries = 512;
		size_t MaximumCpuBytes = 8ull * 1024ull * 1024ull;
		size_t MaximumGpuBytes = 64ull * 1024ull * 1024ull;
		size_t MaximumUploadsPerUpdate = 8;
		size_t MaximumUploadBytesPerUpdate = 512ull * 1024ull;
	};

	struct AssetPreviewTexture
	{
		uint64_t TextureID = 0;
		uint32_t Width = 0;
		uint32_t Height = 0;
	};

	struct AssetPreviewStats
	{
		size_t PendingRequests = 0;
		size_t ReadyEntries = 0;
		size_t FailedEntries = 0;
		// Completed pixel payloads awaiting UI-thread upload; excludes the active worker's source/decode memory.
		size_t QueuedResultCpuBytes = 0;
		size_t GpuBytes = 0;
		size_t ActiveEntries = 0;
		size_t PeakEntries = 0;
		size_t PeakQueuedResultCpuBytes = 0;
		size_t PeakGpuBytes = 0;
		uint64_t CacheHits = 0;
		uint64_t CacheMisses = 0;
		uint64_t DecodesSucceeded = 0;
		uint64_t DecodesFailed = 0;
		uint64_t Uploads = 0;
		uint64_t Evictions = 0;
		uint64_t BindingRegistrations = 0;
		uint64_t BindingUnregistrations = 0;
		uint64_t StaleResultsDiscarded = 0;
		uint64_t CpuBudgetWaits = 0;
		bool WorkerFailed = false;
	};

	class EditorAssetPreviewService final
	{
	public:
		EditorAssetPreviewService() = default;
		EditorAssetPreviewService(const EditorAssetPreviewService&) = delete;
		EditorAssetPreviewService& operator=(const EditorAssetPreviewService&) = delete;
		~EditorAssetPreviewService();

		void Initialize(PulseForge::RenderDevice& Runtime, EditorTextureRegistry& TextureRegistry, AssetPreviewServiceLimits Limits = {});
		void SetProject(std::string ProjectID);
		void BeginRequestFrame();
		[[nodiscard]] bool TouchPreview(std::string_view ProjectID, std::string_view AssetID);
		void RequestImage(std::string_view ProjectID, std::string AssetID, const std::filesystem::path& SourcePath);
		void RequestMaterialSwatch(
			std::string_view ProjectID,
			std::string AssetID,
			const std::filesystem::path& SourcePath);
		void MarkUnavailable(std::string_view ProjectID, std::string AssetID);
		[[nodiscard]] std::optional<AssetPreviewTexture> FindPreview(
			std::string_view ProjectID,
			std::string_view AssetID) const;
		// Call on the service's owning UI/update thread; worker-owned statistics are snapshotted under m_Mutex.
		[[nodiscard]] AssetPreviewStats GetStats() const;
		void PumpUploads();
		void InvalidateAll() noexcept;
		void Shutdown() noexcept;

	private:
		friend struct EditorAssetPreviewServiceTestAccess;
		enum class State : uint8_t { Queued, Decoding, CpuReady, Ready, Failed };
		enum class RequestKind : uint8_t { Image, MaterialSwatch };
		struct Request
		{
			std::string ProjectID;
			std::string AssetID;
			std::filesystem::path SourcePath;
			uint64_t Generation = 0;
			RequestKind Kind = RequestKind::Image;
		};
		struct CpuPreview
		{
			std::string ProjectID;
			std::string AssetID;
			uint64_t Generation = 0;
			uint32_t Width = 0;
			uint32_t Height = 0;
			std::array<uint64_t, 2> Fingerprint{};
			std::vector<uint8_t> Pixels;
			bool IsValid = false;
			AssetPreviewKind Kind = AssetPreviewKind::Image;
		};
		struct Entry
		{
			State Status = State::Queued;
			bool Visible = false;
			uint64_t LastUse = 0;
			uint64_t Generation = 0;
			std::array<uint64_t, 2> Fingerprint{};
			AssetPreviewKey CacheKey;
			uint32_t Width = 0;
			uint32_t Height = 0;
			size_t GpuBytes = 0;
			uint64_t TextureID = 0;
			PulseForge::TextureHandle Texture;
		};

		void WorkerMain() noexcept;
		void MarkWorkerFailed() noexcept;
		[[nodiscard]] static std::optional<CpuPreview> Decode(const Request& Work);
		using DecodeOverrideForTesting = std::optional<CpuPreview> (*)(const Request& Work);
		[[nodiscard]] bool EvictToFit(size_t RequiredBytes, std::string_view ProtectedKey = {}, size_t RequiredEntrySlots = 0);
		void ClearEntries() noexcept;

		PulseForge::RenderDevice* m_Runtime = nullptr;
		EditorTextureRegistry* m_TextureRegistry = nullptr;
		mutable std::mutex m_Mutex;
		std::condition_variable m_WorkAvailable;
		std::deque<Request> m_Requests;
		std::deque<CpuPreview> m_Completed;
		std::unordered_map<std::string, Entry> m_Entries;
		std::jthread m_Worker;
		std::string m_ProjectID;
		uint64_t m_Generation = 1;
		uint64_t m_UseCounter = 0;
		AssetPreviewStats m_Stats;
		std::atomic_uint64_t m_StaleResultsDiscarded = 0;
		std::atomic_bool m_WorkerFailed = false;
		AssetPreviewServiceLimits m_Limits;
		AssetPreviewBudget m_GpuBudget;
		size_t m_QueuedCpuBytes = 0;
		bool m_Stopping = false;
		DecodeOverrideForTesting m_DecodeOverrideForTesting = nullptr;
	};
}
