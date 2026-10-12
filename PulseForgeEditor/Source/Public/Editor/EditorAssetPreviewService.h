#pragma once

#include "Renderer/Texture.h"
#include "Editor/EditorAssetPreviewPolicy.h"

#include <array>
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
	class Application;
}

namespace PulseForgeEditor
{
	class EditorImGuiRenderer;
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
		size_t CpuBytes = 0;
		size_t GpuBytes = 0;
		uint64_t CacheHits = 0;
		uint64_t CacheMisses = 0;
		uint64_t Uploads = 0;
		uint64_t Evictions = 0;
	};

	class EditorAssetPreviewService final
	{
	public:
		EditorAssetPreviewService() = default;
		EditorAssetPreviewService(const EditorAssetPreviewService&) = delete;
		EditorAssetPreviewService& operator=(const EditorAssetPreviewService&) = delete;
		~EditorAssetPreviewService();

		void Initialize(PulseForge::Application& Runtime, EditorImGuiRenderer& Renderer);
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
		[[nodiscard]] AssetPreviewStats GetStats() const;
		void PumpUploads();
		void InvalidateAll() noexcept;
		void Shutdown() noexcept;

	private:
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

		void WorkerMain();
		[[nodiscard]] static std::optional<CpuPreview> Decode(const Request& Work);
		[[nodiscard]] bool EvictToFit(
			size_t RequiredBytes,
			std::string_view ProtectedKey = {},
			size_t RequiredEntrySlots = 0);
		void ClearEntries() noexcept;

		PulseForge::Application* m_Runtime = nullptr;
		EditorImGuiRenderer* m_Renderer = nullptr;
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
		AssetPreviewBudget m_GpuBudget{ 64ull * 1024ull * 1024ull };
		size_t m_QueuedCpuBytes = 0;
		bool m_Stopping = false;
	};
}
