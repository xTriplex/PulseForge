#include "Core/Application.h"
#include "Assets/ImageAssetImporter.h"
#include "Assets/MaterialAsset.h"
#include "Editor/EditorAssetPreviewService.h"
#include "Editor/EditorAssetPreviewPolicy.h"
#include "Editor/EditorImGuiRenderer.h"

#include <algorithm>
#include <fstream>
#include <span>

namespace PulseForgeEditor
{
	namespace
	{
		constexpr uint32_t MaxPreviewEdge = 192;
		constexpr size_t MaxQueuedRequests = 96;
		constexpr size_t MaxEntries = 512;
		constexpr size_t MaxCpuBytes = 8ull * 1024ull * 1024ull;
		constexpr size_t MaxUploadsPerUpdate = 8;
		constexpr size_t MaxUploadBytesPerUpdate = 512ull * 1024ull;

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

	void EditorAssetPreviewService::Initialize(PulseForge::Application& Runtime, EditorImGuiRenderer& Renderer)
	{
		if (m_Worker.joinable())
			return;
		m_Runtime = &Runtime;
		m_Renderer = &Renderer;
		{
			std::scoped_lock Lock(m_Mutex);
			m_Stopping = false;
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
			Asset.Visible = false;
		}
	}

	bool EditorAssetPreviewService::HasEntry(std::string_view ProjectID, std::string_view AssetID) const
	{
		return m_Entries.contains(EntryKey(ProjectID, AssetID));
	}

	void EditorAssetPreviewService::RequestImage(
		std::string_view ProjectID,
		std::string AssetID,
		const std::filesystem::path& SourcePath)
	{
		if (!m_Runtime || !m_Renderer || ProjectID != m_ProjectID || !IsSupportedImagePreviewExtension(SourcePath))
			return;
		const std::string Key = EntryKey(ProjectID, AssetID);
		EvictToFit(0);
		{
			std::scoped_lock Lock(m_Mutex);
			auto Existing = m_Entries.find(Key);
			if (Existing != m_Entries.end())
			{
				++m_Stats.CacheHits;
				Existing->second.Visible = true;
				Existing->second.LastUse = ++m_UseCounter;
				return;
			}
			if (m_Stopping || m_Requests.size() >= MaxQueuedRequests || m_Entries.size() >= MaxEntries)
				return;
			++m_Stats.CacheMisses;
			Entry NewEntry;
			NewEntry.Visible = true;
			NewEntry.LastUse = ++m_UseCounter;
			NewEntry.Generation = m_Generation;
			m_Entries.emplace(Key, std::move(NewEntry));
			m_Requests.push_back({ std::string(ProjectID), std::move(AssetID), SourcePath, m_Generation, RequestKind::Image });
		}
		m_WorkAvailable.notify_one();
	}

	void EditorAssetPreviewService::RequestMaterialSwatch(
		std::string_view ProjectID,
		std::string AssetID,
		const std::filesystem::path& SourcePath)
	{
		if (!m_Runtime || !m_Renderer || ProjectID != m_ProjectID)
			return;
		const std::string Key = EntryKey(ProjectID, AssetID);
		EvictToFit(0);
		{
			std::scoped_lock Lock(m_Mutex);
			if (m_Entries.contains(Key) || m_Stopping || m_Requests.size() >= MaxQueuedRequests ||
				m_Entries.size() >= MaxEntries)
				return;
			++m_Stats.CacheMisses;
			Entry NewEntry;
			NewEntry.Visible = true;
			NewEntry.LastUse = ++m_UseCounter;
			NewEntry.Generation = m_Generation;
			m_Entries.emplace(Key, std::move(NewEntry));
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
		EvictToFit(0);
		std::scoped_lock Lock(m_Mutex);
		if (m_Entries.contains(Key) || m_Stopping || m_Entries.size() >= MaxEntries)
			return;
		Entry FailedEntry;
		FailedEntry.Status = State::Failed;
		FailedEntry.Visible = true;
		FailedEntry.LastUse = ++m_UseCounter;
		FailedEntry.Generation = m_Generation;
		m_Entries.emplace(Key, std::move(FailedEntry));
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
		AssetPreviewStats Result = m_Stats;
		Result.ReadyEntries = 0;
		Result.FailedEntries = 0;
		for (const auto& [Key, Entry] : m_Entries)
		{
			(void)Key;
			Result.ReadyEntries += Entry.Status == State::Ready ? 1 : 0;
			Result.FailedEntries += Entry.Status == State::Failed ? 1 : 0;
		}
		Result.GpuBytes = m_GpuBudget.GetUsed();
		{
			std::scoped_lock Lock(m_Mutex);
			Result.PendingRequests = m_Requests.size();
			Result.CpuBytes = m_QueuedCpuBytes;
		}
		return Result;
	}

	void EditorAssetPreviewService::PumpUploads()
	{
		if (!m_Runtime || !m_Renderer)
			return;
		std::deque<CpuPreview> Completed;
		{
			std::scoped_lock Lock(m_Mutex);
			Completed.swap(m_Completed);
		}
		size_t Uploaded = 0;
		size_t UploadedBytes = 0;
		while (!Completed.empty())
		{
			CpuPreview Preview = std::move(Completed.front());
			Completed.pop_front();
			const size_t ByteCount = Preview.Pixels.size();
			if (Preview.IsValid && (Uploaded >= MaxUploadsPerUpdate ||
				(Uploaded != 0 && UploadedBytes + ByteCount > MaxUploadBytesPerUpdate)))
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
			if (Found == m_Entries.end() || Preview.Generation != m_Generation ||
				Found->second.Generation != Preview.Generation)
			{
				std::scoped_lock Lock(m_Mutex);
				m_QueuedCpuBytes -= (std::min)(m_QueuedCpuBytes, ByteCount);
				continue;
			}
			if (!Preview.IsValid)
			{
				Found->second.Status = State::Failed;
				continue;
			}
			Found->second.Status = State::CpuReady;
			Found->second.Width = Preview.Width;
			Found->second.Height = Preview.Height;
			Found->second.Fingerprint = Preview.Fingerprint;
			Found->second.CacheKey = { Preview.ProjectID, Preview.AssetID, Preview.Fingerprint, Preview.Kind,
				static_cast<uint16_t>(Preview.Width), static_cast<uint16_t>(Preview.Height), 1 };
			EvictToFit(ByteCount);
			if (!m_GpuBudget.CanFit(ByteCount))
			{
				Found->second.Status = State::Failed;
				std::scoped_lock Lock(m_Mutex);
				m_QueuedCpuBytes -= (std::min)(m_QueuedCpuBytes, ByteCount);
				continue;
			}
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
			auto TextureID = m_Renderer->RegisterTexture(**Texture);
			if (!TextureID)
			{
				Found->second.Status = State::Failed;
				std::scoped_lock Lock(m_Mutex);
				m_QueuedCpuBytes -= (std::min)(m_QueuedCpuBytes, ByteCount);
				continue;
			}
			Found->second.Texture = std::move(*Texture);
			Found->second.TextureID = *TextureID;
			Found->second.GpuBytes = ByteCount;
			Found->second.Status = State::Ready;
			(void)m_GpuBudget.Add(ByteCount);
			++m_Stats.Uploads;
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
		if (m_Renderer)
			for (const auto& [Key, Entry] : m_Entries)
			{
				(void)Key;
				if (Entry.TextureID != 0)
					m_Renderer->UnregisterTexture(Entry.TextureID);
			}
		m_Entries.clear();
		m_GpuBudget.Clear();
			m_Stats = {};
	}

	void EditorAssetPreviewService::EvictToFit(size_t RequiredBytes)
	{
		while (!m_GpuBudget.CanFit(RequiredBytes) || m_Entries.size() >= MaxEntries)
		{
			auto Candidate = m_Entries.end();
			for (auto Current = m_Entries.begin(); Current != m_Entries.end(); ++Current)
				if (!Current->second.Visible && Current->second.Status != State::Queued &&
					Current->second.Status != State::Decoding &&
					(Candidate == m_Entries.end() || Current->second.LastUse < Candidate->second.LastUse))
					Candidate = Current;
			if (Candidate == m_Entries.end())
				return;
			if (Candidate->second.TextureID != 0)
				m_Renderer->UnregisterTexture(Candidate->second.TextureID);
			m_GpuBudget.Remove(Candidate->second.GpuBytes);
			m_Entries.erase(Candidate);
			++m_Stats.Evictions;
		}
	}

	void EditorAssetPreviewService::WorkerMain()
	{
		for (;;)
		{
			Request Work;
			{
				std::unique_lock Lock(m_Mutex);
				m_WorkAvailable.wait(Lock, [this] { return m_Stopping || !m_Requests.empty(); });
				if (m_Stopping)
					return;
				Work = std::move(m_Requests.front());
				m_Requests.pop_front();
			}
			auto Preview = Decode(Work);
			std::unique_lock Lock(m_Mutex);
			if (m_Stopping)
				return;
			if (Work.Generation != m_Generation)
				continue;
			if (!Preview)
			{
				CpuPreview Failure;
				Failure.ProjectID = Work.ProjectID;
				Failure.AssetID = Work.AssetID;
				Failure.Generation = Work.Generation;
				m_Completed.push_back(std::move(Failure));
			}
			else
			{
				m_WorkAvailable.wait(Lock, [this, &Preview]
				{
					return m_Stopping || m_QueuedCpuBytes + Preview->Pixels.size() <= MaxCpuBytes;
				});
				if (m_Stopping)
					return;
				if (Work.Generation != m_Generation)
					continue;
				m_QueuedCpuBytes += Preview->Pixels.size();
				m_Completed.push_back(std::move(*Preview));
			}
		}
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
			++m_Generation;
			m_Requests.clear();
			m_Completed.clear();
			m_QueuedCpuBytes = 0;
		}
		m_WorkAvailable.notify_all();
		if (m_Worker.joinable())
			m_Worker.join();
		ClearEntries();
		m_Renderer = nullptr;
		m_Runtime = nullptr;
		m_ProjectID.clear();
	}
}
