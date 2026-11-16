#pragma once

#include "Editor/EditorResourceInventory.h"
#include "Scene/UUID.h"

#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>

namespace PulseForgeEditor
{
	struct EditorResourceInventoryResult
	{
		std::string ProjectIdentity;
		uint64_t Generation = 0;
		std::optional<EditorResourceInventorySnapshot> Snapshot;
		std::string Error;
		bool Cancelled = false;
	};

	// Builds project and derived-data inventory snapshots off the editor thread. Superseded work is cancelled and
	// results are published only for the latest request.
	class EditorResourceInventoryService final
	{
	public:
		using Builder = std::function<EditorResourceInventoryResult(
			const std::filesystem::path&, const PulseForge::UUID&, uint64_t, const EditorResourceInventoryLimits&, std::stop_token)>;

		explicit EditorResourceInventoryService(Builder Build = {});
		~EditorResourceInventoryService();

		EditorResourceInventoryService(const EditorResourceInventoryService&) = delete;
		EditorResourceInventoryService& operator=(const EditorResourceInventoryService&) = delete;

		void RequestRefresh(
			std::filesystem::path ProjectRoot,
			PulseForge::UUID ProjectID,
			uint64_t Generation,
			EditorResourceInventoryLimits Limits = {});
		void Cancel();
		[[nodiscard]] std::optional<EditorResourceInventoryResult> Poll();

	private:
		struct Request
		{
			std::filesystem::path ProjectRoot;
			PulseForge::UUID ProjectID;
			uint64_t Generation = 0;
			uint64_t Serial = 0;
			EditorResourceInventoryLimits Limits;
			std::stop_source Cancellation;
		};

		void WorkerMain(std::stop_token StopToken) noexcept;

		Builder m_Builder;
		std::mutex m_Mutex;
		std::condition_variable_any m_WorkAvailable;
		std::optional<Request> m_Pending;
		std::optional<std::stop_source> m_ActiveCancellation;
		std::optional<EditorResourceInventoryResult> m_Completed;
		uint64_t m_LatestSerial = 0;
		uint64_t m_ActiveSerial = 0;
		bool m_Stopping = false;
		std::jthread m_Worker;
	};
} // namespace PulseForgeEditor
