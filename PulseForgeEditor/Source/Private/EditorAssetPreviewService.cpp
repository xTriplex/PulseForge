#include "Renderer/RenderDevice.h"
#include "Assets/ImageAssetImporter.h"
#include "Assets/MaterialAsset.h"
#include "Editor/EditorAssetPreviewService.h"
#include "Editor/EditorAssetPreviewPolicy.h"

#include <fstream>
#include <span>

namespace PulseForgeEditor
{
	namespace
	{
		constexpr uint32_t MaxPreviewEdge = 192;

		std::string EntryKey(std::string_view ProjectID, std::string_view AssetID)
		{
			std::string Key;
			Key.reserve(ProjectID.size() + AssetID.size() + 1);
			Key.append(ProjectID);
			Key.push_back('\0');
			Key.append(AssetID);
			return Key;
		}
	}

	EditorAssetPreviewService::~EditorAssetPreviewService()
	{
		Shutdown();
	}

	void EditorAssetPreviewService::Initialize(
		PulseForge::RenderDevice& Runtime, EditorTextureRegistry& TextureRegistry, AssetPreviewServiceLimits Limits)
	{
		if (m_Worker.joinable()) return;
		m_Runtime = &Runtime;
		m_TextureRegistry = &TextureRegistry;
		m_Limits = Limits;
		m_GpuBudget.SetLimit(Limits.MaximumGpuBytes);
		{
			std::scoped_lock Lock(m_Mutex);
			m_Stopping = false;
			m_WorkerFailed.store(false, std::memory_order_release);
		}
		m_Worker = std::jthread([this] { WorkerMain(); });
	}

	void EditorAssetPreviewService::SetProject(std::string ProjectID)
	{
		if (ProjectID == m_ProjectID)
			return;
		InvalidateAll();
		m_ProjectID = std::move(ProjectID);
	}

	void EditorAssetPreviewService::BeginRequestFrame()
	{
		for (auto& [Key, Asset] : m_Entries)
		{
			(void)Key;
			BeginAssetPreviewFrame(Asset.Visible);
		}
	}

	bool EditorAssetPreviewService::TouchPreview(std::string_view ProjectID, std::string_view AssetID)
	{
		const auto Found = m_Entries.find(EntryKey(ProjectID, AssetID));
		if (Found == m_Entries.end())
			return false;
		++m_Stats.CacheHits;
		TouchAssetPreviewEntry(Found->second.Visible, Found->second.LastUse, m_UseCounter);
		return true;
	}

	void EditorAssetPreviewService::RequestImage(std::string_view ProjectID, std::string AssetID, const std::filesystem::path& SourcePath)
	{
		if (!m_Runtime || !m_TextureRegistry || ProjectID != m_ProjectID || !IsSupportedImagePreviewExtension(SourcePath)) return;
		const std::string Key = EntryKey(ProjectID, AssetID);
		{
			std::scoped_lock Lock(m_Mutex);
			auto Existing = m_Entries.find(Key);
			if (Existing != m_Entries.end())
			{
				++m_Stats.CacheHits;
				TouchAssetPreviewEntry(Existing->second.Visible, Existing->second.LastUse, m_UseCounter);
				return;
			}
			if (m_Stopping || m_WorkerFailed.load(std::memory_order_acquire) || m_Requests.size() >= m_Limits.MaximumQueuedRequests) return;
		}
		if (!EvictToFit(0, Key, 1))
			return;
		{
			std::scoped_lock Lock(m_Mutex);
			if (m_Stopping || m_WorkerFailed.load(std::memory_order_acquire) || m_Requests.size() >= m_Limits.MaximumQueuedRequests ||
				m_Entries.contains(Key))
				return;
			++m_Stats.CacheMisses;
			Entry NewEntry;
			TouchAssetPreviewEntry(NewEntry.Visible, NewEntry.LastUse, m_UseCounter);
			NewEntry.Generation = m_Generation;
			m_Entries.emplace(Key, std::move(NewEntry));
			m_Stats.PeakEntries = (std::max)(m_Stats.PeakEntries, m_Entries.size());
			m_Requests.push_back({ std::string(ProjectID), std::move(AssetID), SourcePath, m_Generation, RequestKind::Image });
		}
		m_WorkAvailable.notify_one();
	}

	void EditorAssetPreviewService::RequestMaterialSwatch(
		std::string_view ProjectID, std::string AssetID, const std::filesystem::path& SourcePath)
	{
		if (!m_Runtime || !m_TextureRegistry || ProjectID != m_ProjectID) return;
		const std::string Key = EntryKey(ProjectID, AssetID);
		{
			std::scoped_lock Lock(m_Mutex);
			auto Existing = m_Entries.find(Key);
			if (Existing != m_Entries.end())
			{
				++m_Stats.CacheHits;
				TouchAssetPreviewEntry(Existing->second.Visible, Existing->second.LastUse, m_UseCounter);
				return;
			}
			if (m_Stopping || m_WorkerFailed.load(std::memory_order_acquire) || m_Requests.size() >= m_Limits.MaximumQueuedRequests) return;
		}
		if (!EvictToFit(0, Key, 1))
			return;
		{
			std::scoped_lock Lock(m_Mutex);
			if (m_Stopping || m_WorkerFailed.load(std::memory_order_acquire) || m_Requests.size() >= m_Limits.MaximumQueuedRequests ||
				m_Entries.contains(Key))
				return;
			++m_Stats.CacheMisses;
			Entry NewEntry;
			TouchAssetPreviewEntry(NewEntry.Visible, NewEntry.LastUse, m_UseCounter);
			NewEntry.Generation = m_Generation;
			m_Entries.emplace(Key, std::move(NewEntry));
			m_Stats.PeakEntries = (std::max)(m_Stats.PeakEntries, m_Entries.size());
			m_Requests.push_back({
				std::string(ProjectID), std::move(AssetID), SourcePath, m_Generation, RequestKind::MaterialSwatch });
		}
		m_WorkAvailable.notify_one();
	}

	void EditorAssetPreviewService::MarkUnavailable(std::string_view ProjectID, std::string AssetID)
	{
		if (ProjectID != m_ProjectID)
			return;
		const std::string Key = EntryKey(ProjectID, AssetID);
		{
			std::scoped_lock Lock(m_Mutex);
			auto Existing = m_Entries.find(Key);
			if (Existing != m_Entries.end())
			{
				TouchAssetPreviewEntry(Existing->second.Visible, Existing->second.LastUse, m_UseCounter);
				return;
			}
			if (m_Stopping || m_WorkerFailed.load(std::memory_order_acquire)) return;
		}
		if (!EvictToFit(0, Key, 1))
			return;
		std::scoped_lock Lock(m_Mutex);
		if (m_Entries.contains(Key) || m_Stopping || m_WorkerFailed.load(std::memory_order_acquire)) return;
		Entry FailedEntry;
		FailedEntry.Status = State::Failed;
		TouchAssetPreviewEntry(FailedEntry.Visible, FailedEntry.LastUse, m_UseCounter);
		FailedEntry.Generation = m_Generation;
		m_Entries.emplace(Key, std::move(FailedEntry));
		m_Stats.PeakEntries = (std::max)(m_Stats.PeakEntries, m_Entries.size());
	}

	std::optional<AssetPreviewTexture> EditorAssetPreviewService::FindPreview(
		std::string_view ProjectID,
		std::string_view AssetID) const
	{
		const auto Found = m_Entries.find(EntryKey(ProjectID, AssetID));
		if (Found == m_Entries.end() || Found->second.Status != State::Ready)
			return std::nullopt;
		return AssetPreviewTexture{ Found->second.TextureID, Found->second.Width, Found->second.Height };
	}

	AssetPreviewStats EditorAssetPreviewService::GetStats() const
	{
		AssetPreviewStats Result;
		{
			std::scoped_lock Lock(m_Mutex);
			Result = m_Stats;
			Result.PendingRequests = m_Requests.size();
			Result.QueuedResultCpuBytes = m_QueuedCpuBytes;
		}
		Result.StaleResultsDiscarded = m_StaleResultsDiscarded.load(std::memory_order_relaxed);
		Result.WorkerFailed = m_WorkerFailed.load(std::memory_order_acquire);
		Result.ReadyEntries = 0;
		Result.FailedEntries = 0;
		for (const auto& [Key, Entry] : m_Entries)
		{
			(void)Key;
			Result.ReadyEntries += Entry.Status == State::Ready ? 1 : 0;
			Result.FailedEntries += Entry.Status == State::Failed ? 1 : 0;
		}
		Result.GpuBytes = m_GpuBudget.GetUsed();
		Result.ActiveEntries = m_Entries.size();
		Result.PeakGpuBytes = m_Stats.PeakGpuBytes;
		return Result;
	}

	void EditorAssetPreviewService::PumpUploads()
	{
		if (!m_Runtime || !m_TextureRegistry) return;
		std::deque<CpuPreview> Completed;
		{
			std::scoped_lock Lock(m_Mutex);
			if (m_WorkerFailed.load(std::memory_order_acquire))
			{
				m_Requests.clear();
				m_Completed.clear();
				m_QueuedCpuBytes = 0;
				for (auto& [Key, Entry] : m_Entries)
				{
					(void)Key;
					if (Entry.Status == State::Queued || Entry.Status == State::Decoding || Entry.Status == State::CpuReady)
						Entry.Status = State::Failed;
				}
				return;
			}
			Completed.swap(m_Completed);
		}
		size_t Uploaded = 0;
		size_t UploadedBytes = 0;
		while (!Completed.empty())
		{
			CpuPreview Preview = std::move(Completed.front());
			Completed.pop_front();
			const size_t ByteCount = Preview.Pixels.size();
			if (Preview.IsValid && m_Limits.MaximumUploadsPerUpdate != 0 && m_Limits.MaximumUploadBytesPerUpdate != 0 &&
				ByteCount <= m_Limits.MaximumUploadBytesPerUpdate &&
				(Uploaded >= m_Limits.MaximumUploadsPerUpdate || UploadedBytes > m_Limits.MaximumUploadBytesPerUpdate - ByteCount))
			{
				std::scoped_lock Lock(m_Mutex);
				m_Completed.push_back(std::move(Preview));
				while (!Completed.empty())
				{
					m_Completed.push_back(std::move(Completed.front()));
					Completed.pop_front();
				}
				break;
			}
			const std::string Key = EntryKey(Preview.ProjectID, Preview.AssetID);
			auto Found = m_Entries.find(Key);
			if (Found == m_Entries.end() || !IsCurrentAssetPreviewResult(
				Preview.Generation, Found->second.Generation, m_Generation, m_Stopping))
			{
				m_StaleResultsDiscarded.fetch_add(1, std::memory_order_relaxed);
				std::scoped_lock Lock(m_Mutex);
				m_QueuedCpuBytes -= (std::min)(m_QueuedCpuBytes, ByteCount);
				continue;
			}
			if (!Preview.IsValid)
			{
				Found->second.Status = State::Failed;
				++m_Stats.DecodesFailed;
				continue;
			}
			if (m_Limits.MaximumUploadsPerUpdate == 0 || m_Limits.MaximumUploadBytesPerUpdate == 0 || ByteCount == 0 ||
				ByteCount > m_Limits.MaximumUploadBytesPerUpdate)
			{
				Found->second.Status = State::Failed;
				++m_Stats.DecodesFailed;
				std::scoped_lock Lock(m_Mutex);
				m_QueuedCpuBytes -= (std::min)(m_QueuedCpuBytes, ByteCount);
				continue;
			}
			++m_Stats.DecodesSucceeded;
			if (!EvictToFit(ByteCount, Key))
			{
				Found = m_Entries.find(Key);
				if (Found != m_Entries.end())
					Found->second.Status = State::Failed;
				std::scoped_lock Lock(m_Mutex);
				m_QueuedCpuBytes -= (std::min)(m_QueuedCpuBytes, ByteCount);
				continue;
			}
			Found = m_Entries.find(Key);
			if (Found == m_Entries.end() || !IsCurrentAssetPreviewResult(
				Preview.Generation, Found->second.Generation, m_Generation, m_Stopping))
			{
				std::scoped_lock Lock(m_Mutex);
				m_QueuedCpuBytes -= (std::min)(m_QueuedCpuBytes, ByteCount);
				continue;
			}
			Found->second.Status = State::CpuReady;
			Found->second.Width = Preview.Width;
			Found->second.Height = Preview.Height;
			Found->second.Fingerprint = Preview.Fingerprint;
			Found->second.CacheKey = { Preview.ProjectID, Preview.AssetID, Preview.Fingerprint, Preview.Kind,
				static_cast<uint16_t>(Preview.Width), static_cast<uint16_t>(Preview.Height), 1 };
			PulseForge::TextureDesc Description;
			Description.Width = Preview.Width;
			Description.Height = Preview.Height;
			Description.Format = PulseForge::TextureFormat::RGBA8_Srgb;
			Description.Usage = PulseForge::TextureUsage::ShaderResource;
			Description.DebugName = "PulseForge Content Browser image preview";
			const std::span<const std::byte> PixelData(
				reinterpret_cast<const std::byte*>(Preview.Pixels.data()), Preview.Pixels.size());
			auto Texture = m_Runtime->CreateTexture(Description, PixelData);
			if (!Texture)
			{
				Found->second.Status = State::Failed;
				std::scoped_lock Lock(m_Mutex);
				m_QueuedCpuBytes -= (std::min)(m_QueuedCpuBytes, ByteCount);
				continue;
			}
			auto TextureID = m_TextureRegistry->RegisterTexture(**Texture);
			if (!TextureID)
			{
				Found->second.Status = State::Failed;
				std::scoped_lock Lock(m_Mutex);
				m_QueuedCpuBytes -= (std::min)(m_QueuedCpuBytes, ByteCount);
				continue;
			}
			if (!m_GpuBudget.Add(ByteCount))
			{
				m_TextureRegistry->UnregisterTexture(*TextureID);
				Found->second.Status = State::Failed;
				std::scoped_lock Lock(m_Mutex);
				m_QueuedCpuBytes -= (std::min)(m_QueuedCpuBytes, ByteCount);
				continue;
			}
			Found->second.Texture = std::move(*Texture);
			Found->second.TextureID = *TextureID;
			Found->second.GpuBytes = ByteCount;
			Found->second.Status = State::Ready;
			++m_Stats.Uploads;
			++m_Stats.BindingRegistrations;
			m_Stats.PeakGpuBytes = (std::max)(m_Stats.PeakGpuBytes, m_GpuBudget.GetUsed());
			++Uploaded;
			UploadedBytes += ByteCount;
			{
				std::scoped_lock Lock(m_Mutex);
				m_QueuedCpuBytes -= (std::min)(m_QueuedCpuBytes, ByteCount);
			}
		}
		m_WorkAvailable.notify_one();
	}

	void EditorAssetPreviewService::InvalidateAll() noexcept
	{
		{
			std::scoped_lock Lock(m_Mutex);
			m_StaleResultsDiscarded.fetch_add(m_Requests.size() + m_Completed.size(), std::memory_order_relaxed);
			++m_Generation;
			m_Requests.clear();
			m_Completed.clear();
			m_QueuedCpuBytes = 0;
		}
		ClearEntries();
		m_WorkAvailable.notify_all();
	}

	void EditorAssetPreviewService::ClearEntries() noexcept
	{
		if (m_TextureRegistry)
			for (const auto& [Key, Entry] : m_Entries)
			{
				(void)Key;
				if (Entry.TextureID != 0)
				{
					m_TextureRegistry->UnregisterTexture(Entry.TextureID);
					++m_Stats.BindingUnregistrations;
				}
			}
		m_Entries.clear();
		m_GpuBudget.Clear();
	}

	bool EditorAssetPreviewService::EvictToFit(
		size_t RequiredBytes,
		std::string_view ProtectedKey,
		size_t RequiredEntrySlots)
	{
		std::vector<AssetPreviewEvictionCandidate> Candidates;
		Candidates.reserve(m_Entries.size());
		for (const auto& [Key, Asset] : m_Entries)
		{
			const bool InFlight = Asset.Status == State::Queued || Asset.Status == State::Decoding ||
				Asset.Status == State::CpuReady;
			Candidates.push_back({ Key, Asset.Visible, InFlight, Asset.LastUse, Asset.GpuBytes });
		}
		auto Planned = PlanAssetPreviewEvictions(Candidates, m_Entries.size(), m_Limits.MaximumEntries,
			m_GpuBudget.GetUsed(), m_GpuBudget.GetLimit(), RequiredBytes, RequiredEntrySlots, ProtectedKey);
		if (!Planned)
			return false;
		for (const std::string& Key : *Planned)
		{
			auto Candidate = m_Entries.find(Key);
			if (Candidate == m_Entries.end()) continue;
			if (Candidate->second.TextureID != 0)
			{
				m_TextureRegistry->UnregisterTexture(Candidate->second.TextureID);
				++m_Stats.BindingUnregistrations;
			}
			m_GpuBudget.Remove(Candidate->second.GpuBytes);
			m_Entries.erase(Candidate);
			++m_Stats.Evictions;
		}
		return m_GpuBudget.CanFit(RequiredBytes) &&
			m_Entries.size() + RequiredEntrySlots <= m_Limits.MaximumEntries;
	}

	void EditorAssetPreviewService::WorkerMain() noexcept
	{
		try
		{
			for (;;)
			{
				Request Work;
				{
					std::unique_lock Lock(m_Mutex);
					m_WorkAvailable.wait(Lock, [this] { return m_Stopping || !m_Requests.empty(); });
					if (m_Stopping || m_WorkerFailed.load(std::memory_order_acquire)) return;
					Work = std::move(m_Requests.front());
					m_Requests.pop_front();
				}
				std::optional<CpuPreview> Preview;
				try
				{
					Preview = m_DecodeOverrideForTesting ? m_DecodeOverrideForTesting(Work) : Decode(Work);
				}
				catch (...)
				{
					// A single malformed/unavailable decode must fail its entry without terminating the worker.
					Preview.reset();
				}

				std::unique_lock Lock(m_Mutex);
				if (m_Stopping || m_WorkerFailed.load(std::memory_order_acquire)) return;
				if (!IsCurrentAssetPreviewResult(Work.Generation, Work.Generation, m_Generation, m_Stopping))
				{
					m_StaleResultsDiscarded.fetch_add(1, std::memory_order_relaxed);
					continue;
				}

				if (!Preview || !Preview->IsValid || Preview->Pixels.empty() || Preview->Pixels.size() > m_Limits.MaximumCpuBytes)
				{
					CpuPreview Failure;
					Failure.ProjectID = Preview ? std::move(Preview->ProjectID) : std::move(Work.ProjectID);
					Failure.AssetID = Preview ? std::move(Preview->AssetID) : std::move(Work.AssetID);
					Failure.Generation = Work.Generation;
					m_Completed.push_back(std::move(Failure));
					continue;
				}

				const size_t ResultBytes = Preview->Pixels.size();
				if (m_QueuedCpuBytes > m_Limits.MaximumCpuBytes - ResultBytes) ++m_Stats.CpuBudgetWaits;
				m_WorkAvailable.wait(Lock,
					[this, &Work, ResultBytes]
					{
						return m_Stopping || Work.Generation != m_Generation || m_QueuedCpuBytes <= m_Limits.MaximumCpuBytes - ResultBytes;
					});
				if (m_Stopping || m_WorkerFailed.load(std::memory_order_acquire)) return;
				if (Work.Generation != m_Generation)
				{
					m_StaleResultsDiscarded.fetch_add(1, std::memory_order_relaxed);
					continue;
				}
				m_QueuedCpuBytes += ResultBytes;
				m_Stats.PeakQueuedResultCpuBytes = (std::max)(m_Stats.PeakQueuedResultCpuBytes, m_QueuedCpuBytes);
				m_Completed.push_back(std::move(*Preview));
			}
		}
		catch (...)
		{
			MarkWorkerFailed();
		}
	}

	void EditorAssetPreviewService::MarkWorkerFailed() noexcept
	{
		m_WorkerFailed.store(true, std::memory_order_release);
		try
		{
			std::scoped_lock Lock(m_Mutex);
			m_StaleResultsDiscarded.fetch_add(m_Requests.size() + m_Completed.size(), std::memory_order_relaxed);
			m_Requests.clear();
			m_Completed.clear();
			m_QueuedCpuBytes = 0;
		}
		catch (...)
		{
			// Keep the thread boundary non-throwing even if synchronization itself fails.
		}
		m_WorkAvailable.notify_all();
	}

	std::optional<EditorAssetPreviewService::CpuPreview> EditorAssetPreviewService::Decode(const Request& Work)
	{
		if (Work.Kind == RequestKind::MaterialSwatch)
		{
			std::error_code FileError;
			const uintmax_t FileSize = std::filesystem::file_size(Work.SourcePath, FileError);
			if (FileError || FileSize == 0 || FileSize > 1024ull * 1024ull)
				return std::nullopt;
			std::string Source(static_cast<size_t>(FileSize), '\0');
			std::ifstream Input(Work.SourcePath, std::ios::binary);
			if (!Input || !Input.read(Source.data(), static_cast<std::streamsize>(Source.size())))
				return std::nullopt;
			auto Material = PulseForge::MaterialAssetSerializer::Deserialize(Source);
			if (!Material)
				return std::nullopt;
			CpuPreview Result;
			Result.ProjectID = Work.ProjectID;
			Result.AssetID = Work.AssetID;
			Result.Generation = Work.Generation;
			Result.Width = 1;
			Result.Height = 1;
			const auto Swatch = MakeMaterialBaseColorSwatch(Material->BaseColorFactor.r,
				Material->BaseColorFactor.g, Material->BaseColorFactor.b, Material->BaseColorFactor.a,
				Material->AlphaMode == PulseForge::MaterialAlphaMode::Opaque);
			Result.Pixels.assign(Swatch.begin(), Swatch.end());
			Result.Kind = AssetPreviewKind::MaterialSwatch;
			uint64_t HashA = 14695981039346656037ull;
			uint64_t HashB = 7809847782465536322ull;
			for (const unsigned char Byte : Source)
			{
				HashA = (HashA ^ Byte) * 1099511628211ull;
				HashB = (HashB ^ (Byte + 0x9du)) * 14029467366897019727ull;
				HashB ^= HashB >> 29;
			}
			Result.Fingerprint = { HashA, HashB };
			Result.IsValid = true;
			return Result;
		}
		auto Decoded = PulseForge::ImageAssetImporter::DecodePreviewRGBA8(Work.SourcePath, MaxPreviewEdge);
		if (!Decoded)
			return std::nullopt;
		CpuPreview Result;
		Result.ProjectID = Work.ProjectID;
		Result.AssetID = Work.AssetID;
		Result.Generation = Work.Generation;
		Result.Width = Decoded->Width;
		Result.Height = Decoded->Height;
		Result.Fingerprint = Decoded->SourceFingerprint;
		Result.Pixels = std::move(Decoded->RGBA8Pixels);
		Result.Kind = AssetPreviewKind::Image;
		Result.IsValid = true;
		return Result;
	}

	void EditorAssetPreviewService::Shutdown() noexcept
	{
		{
			std::scoped_lock Lock(m_Mutex);
			m_Stopping = true;
			m_StaleResultsDiscarded.fetch_add(m_Requests.size() + m_Completed.size(), std::memory_order_relaxed);
			++m_Generation;
			m_Requests.clear();
			m_Completed.clear();
			m_QueuedCpuBytes = 0;
		}
		m_WorkAvailable.notify_all();
		if (m_Worker.joinable()) m_Worker.join();
		ClearEntries();
		m_TextureRegistry = nullptr;
		m_Runtime = nullptr;
		m_ProjectID.clear();
	}
}
