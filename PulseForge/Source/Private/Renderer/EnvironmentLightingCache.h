#pragma once

#include "Assets/AssetRegistry.h"
#include "Renderer/EnvironmentLighting.h"
#include "Renderer/RenderDevice.h"
#include "Renderer/EnvironmentLightingTextures.h"

#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>

namespace PulseForge
{
	struct EnvironmentLightingCacheError
	{
		AssetID Asset;
		std::string Message;
	};

	// CPU decoding/preprocessing is asynchronous; GPU upload and texture ownership stay on the renderer thread.
	class EnvironmentLightingCache final
	{
	public:
		EnvironmentLightingCache(RenderDevice& Runtime, std::filesystem::path ProjectRoot, const AssetRegistry& Registry);
		~EnvironmentLightingCache();
		EnvironmentLightingCache(const EnvironmentLightingCache&) = delete;
		EnvironmentLightingCache& operator=(const EnvironmentLightingCache&) = delete;
		[[nodiscard]] std::expected<std::optional<std::reference_wrapper<const EnvironmentLightingTextures>>, EnvironmentLightingCacheError>
		GetOrLoad(const AssetID& Asset);
		[[nodiscard]] size_t GetLoadedCount() const noexcept { return m_Entries.size(); }
		void Clear() noexcept;

	private:
		enum class EntryState : uint8_t
		{
			Queued,
			Processing,
			CpuReady,
			Uploading,
			Ready,
			Failed
		};
		struct Entry
		{
			mutable std::mutex Mutex;
			EntryState State = EntryState::Queued;
			std::optional<EnvironmentLightingData> CpuData;
			std::optional<EnvironmentLightingTextures> GpuTextures;
			std::string Error;
		};
		struct Request
		{
			AssetID Asset;
			std::filesystem::path ProjectRoot;
			AssetRegistry Registry;
			std::shared_ptr<Entry> Destination;
		};

		void WorkerMain(std::stop_token StopToken) noexcept;
		[[nodiscard]] std::expected<EnvironmentLightingTextures, EnvironmentLightingCacheError> UploadProcessedData(
			const AssetID& Asset, EnvironmentLightingData Data);

		RenderDevice& m_Runtime;
		std::filesystem::path m_ProjectRoot;
		const AssetRegistry& m_Registry;
		std::unordered_map<AssetID, std::shared_ptr<Entry>, UUIDHash> m_Entries;
		std::deque<Request> m_Requests;
		std::mutex m_QueueMutex;
		std::condition_variable_any m_QueueChanged;
		std::jthread m_Worker;
	};
} // namespace PulseForge
