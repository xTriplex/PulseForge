#include "Renderer/AssetCache/TextureAssetCache.h"

#if !defined(PF_COOKED_RUNTIME_ONLY)
#include "Assets/ImageAssetImporter.h"
#include "Assets/AssetPathResolver.h"
#include "Assets/DerivedDataArtifactStore.h"
#include "Assets/Detail/AssetCpuProducts.h"
#endif
#include "Core/Diagnostics.h"
#include "Runtime/CookedContentProvider.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace PulseForge
{
	namespace
	{
		using OwnedMipLevel = Detail::PreparedMipLevel;
#if !defined(PF_COOKED_RUNTIME_ONLY)
		struct PreparedTextureData
		{
			uint32_t Width = 0;
			uint32_t Height = 0;
			TextureFormat Format = TextureFormat::RGBA8_Srgb;
			std::vector<OwnedMipLevel> MipLevels;
		};

		// Peak includes bounded source bytes, decoder output, retained mip levels, and an encoded cache-write buffer.
		constexpr size_t EstimatedTextureWorkingBytes = 480ull * 1024ull * 1024ull;

		template <typename Product>
		size_t GetPreparedTextureRetainedBytes(const Product& Data) noexcept
		{
			size_t Total = 0;
			for (const OwnedMipLevel& Level : Data.MipLevels)
			{
				if (Level.Pixels.size() > (std::numeric_limits<size_t>::max)() - Total)
					return (std::numeric_limits<size_t>::max)();
				Total += Level.Pixels.size();
			}
			return Total;
		}
#endif

	} // namespace

#if !defined(PF_COOKED_RUNTIME_ONLY)
	TextureAssetCache::TextureAssetCache(
		RenderDevice& Runtime,
		std::filesystem::path ProjectRoot,
		const AssetRegistry& Registry,
		DiagnosticRecorder* Diagnostics)
		: m_Runtime(Runtime), m_ProjectRoot(std::move(ProjectRoot)),
		  m_RegistryOwner(std::make_shared<const AssetRegistry>(Registry)), m_Registry(m_RegistryOwner.get()), m_Diagnostics(Diagnostics)
	{
	}
#endif

	TextureAssetCache::TextureAssetCache(
		RenderDevice& Runtime, std::shared_ptr<const CookedContentProvider> Content, DiagnosticRecorder* Diagnostics)
		: m_Runtime(Runtime), m_CookedContent(std::move(Content)), m_Diagnostics(Diagnostics)
	{
		if (!m_CookedContent)
			throw std::invalid_argument("Cooked texture cache requires a content provider");
	}

#if !defined(PF_COOKED_RUNTIME_ONLY)
	TextureAssetCache::TextureAssetCache(
		RenderDevice& Runtime,
		std::filesystem::path ProjectRoot,
		const AssetRegistry& Registry,
		AssetPreparationService& Preparation,
		AssetUploadBudget* UploadBudget,
		DerivedDataArtifactStore& ArtifactStore,
		std::string SessionKey,
		DiagnosticRecorder* Diagnostics)
		: m_Runtime(Runtime), m_ProjectRoot(std::move(ProjectRoot)),
		  m_RegistryOwner(std::make_shared<const AssetRegistry>(Registry)), m_Registry(m_RegistryOwner.get()),
		  m_Preparation(&Preparation), m_UploadBudget(UploadBudget), m_ArtifactStore(&ArtifactStore),
		  m_SessionKey(std::move(SessionKey)), m_Diagnostics(Diagnostics)
	{
	}

	TextureAssetCache::TextureAssetCache(
		RenderDevice& Runtime,
		std::filesystem::path ProjectRoot,
		std::shared_ptr<const AssetRegistry> Registry,
		AssetPreparationService& Preparation,
		AssetUploadBudget* UploadBudget,
		DerivedDataArtifactStore& ArtifactStore,
		std::string SessionKey,
		DiagnosticRecorder* Diagnostics)
		: m_Runtime(Runtime), m_ProjectRoot(std::move(ProjectRoot)), m_RegistryOwner(std::move(Registry)),
		  m_Registry(m_RegistryOwner.get()), m_Preparation(&Preparation), m_UploadBudget(UploadBudget),
		  m_ArtifactStore(&ArtifactStore), m_SessionKey(std::move(SessionKey)), m_Diagnostics(Diagnostics)
	{
	}
#endif

	TextureAssetCache::~TextureAssetCache()
	{
		Clear();
	}

	void TextureAssetCache::SetSessionKey(std::string SessionKey)
	{
		if (SessionKey == m_SessionKey)
			return;
		m_SessionKey = std::move(SessionKey);
		for (auto& [Key, Entry] : m_Textures)
		{
			(void)Key;
			Entry.Pending.reset();
			Entry.SessionKey.clear();
			Entry.FailureMessage.clear();
			Entry.TerminalFailure = false;
			Entry.TerminalFailureCode = TextureAssetErrorCode::TextureCreationFailed;
		}
	}

	void TextureAssetCache::SetRegistrySnapshot(std::shared_ptr<const AssetRegistry> Registry) noexcept
	{
		if (!Registry)
			return;
		m_RegistryOwner = std::move(Registry);
		m_Registry = m_RegistryOwner.get();
	}

	std::expected<std::reference_wrapper<const Texture>, TextureAssetError> TextureAssetCache::GetOrLoad(
		const AssetID& Asset, TextureFormat Format)
	{
		return GetOrLoadInternal(Asset, Format, false);
	}

	std::expected<std::reference_wrapper<const Texture>, TextureAssetError> TextureAssetCache::GetOrLoadInternal(
		const AssetID& Asset, TextureFormat Format, bool CollectReadyProduct)
	{
		if (m_CookedContent)
		{
			if (Format != TextureFormat::RGBA8_UNorm && Format != TextureFormat::RGBA8_Srgb)
				return std::unexpected(TextureAssetError{ TextureAssetErrorCode::UnsupportedFormat, Asset,
					"Image assets can only be loaded as RGBA8_UNorm or RGBA8_Srgb textures" });
			if (Asset.IsNil())
				return std::unexpected(TextureAssetError{ TextureAssetErrorCode::AssetNotFound, Asset,
					"Cannot load a texture with a nil asset UUID" });
			const CacheKey Key{ Asset, Format };
			auto [EntryIterator, WasInserted] = m_Textures.try_emplace(Key);
			(void)WasInserted;
			Entry& State = EntryIterator->second;
			if (State.Current)
			{
				++m_CacheHits;
				return std::cref(*State.Current);
			}
			++m_CacheMisses;
			const std::string_view Subresource = Format == TextureFormat::RGBA8_Srgb ? "srgb" : "linear";
			auto Product = m_CookedContent->LoadTextureProduct(Asset, Subresource);
			if (!Product)
				return std::unexpected(TextureAssetError{
					Product.error().Code == CookedAssetCatalogErrorCode::AssetNotFound
						? TextureAssetErrorCode::AssetNotFound : TextureAssetErrorCode::ImportFailed,
					Asset, Product.error().Message });
			const auto& Data = *Product;
			const Detail::ImageColorSpace ExpectedColorSpace = Format == TextureFormat::RGBA8_Srgb
				? Detail::ImageColorSpace::Srgb : Detail::ImageColorSpace::Linear;
			if (Data.ColorSpace != ExpectedColorSpace)
				return std::unexpected(TextureAssetError{ TextureAssetErrorCode::ImportFailed, Asset,
					"Cooked texture color space does not match its requested subresource" });
			TextureDesc Description;
			Description.Width = Data.Width;
			Description.Height = Data.Height;
			Description.Format = Data.ColorSpace == Detail::ImageColorSpace::Srgb
				? TextureFormat::RGBA8_Srgb : TextureFormat::RGBA8_UNorm;
			Description.MipLevels = static_cast<uint32_t>(Data.MipLevels.size());
			Description.DebugName = "Image asset " + Asset.ToString();
			std::vector<TextureSubresourceData> InitialData;
			InitialData.reserve(Data.MipLevels.size());
			for (uint32_t MipLevel = 0; MipLevel < Data.MipLevels.size(); ++MipLevel)
			{
				const OwnedMipLevel& Level = Data.MipLevels[MipLevel];
				InitialData.push_back({ MipLevel, 0, std::as_bytes(std::span(Level.Pixels)), 0 });
			}
			auto CreatedTexture = m_Runtime.CreateTexture(Description, InitialData);
			if (!CreatedTexture)
				return std::unexpected(TextureAssetError{ TextureAssetErrorCode::TextureCreationFailed, Asset,
					CreatedTexture.error().Message });
			State.Current = std::move(*CreatedTexture);
			++State.ResourceRevision;
			if (m_Diagnostics)
				(void)m_Diagnostics->RecordEvent(DiagnosticEventKind::Resource, "Texture cache resource created", "TextureAssetCache", "objects", 1);
			return std::cref(*State.Current);
		}
#if defined(PF_COOKED_RUNTIME_ONLY)
		(void)CollectReadyProduct;
		return std::unexpected(TextureAssetError{ TextureAssetErrorCode::AssetNotFound, Asset,
			"No cooked texture product is available for this asset identity" });
#else
		if (Asset.IsNil() || !m_Registry->Find(Asset))
		{
			return std::unexpected(TextureAssetError{ TextureAssetErrorCode::AssetNotFound, Asset,
				Asset.IsNil() ? "Cannot load a texture with a nil asset UUID"
							  : "Texture asset UUID " + Asset.ToString() + " is not present in the project asset registry" });
		}

		if (Format != TextureFormat::RGBA8_UNorm && Format != TextureFormat::RGBA8_Srgb)
		{
			return std::unexpected(TextureAssetError{
				TextureAssetErrorCode::UnsupportedFormat, Asset, "Image assets can only be loaded as RGBA8_UNorm or RGBA8_Srgb textures" });
		}

		const CacheKey Key{ Asset, Format };
		auto [EntryIterator, WasInserted] = m_Textures.try_emplace(Key);
		(void)WasInserted;
		Entry& State = EntryIterator->second;
		if (!m_Preparation)
		{
			if (State.Current)
			{
				++m_CacheHits;
				return std::cref(*State.Current);
			}
			++m_CacheMisses;
			auto ImportedImage = [&]()
			{
				ScopedDiagnosticTimer Timer(m_Diagnostics, "Texture asset import", "TextureAssetCache");
				return ImageAssetImporter::ImportRGBA8(Asset, m_ProjectRoot, *m_Registry);
			}();
			if (!ImportedImage)
				return std::unexpected(TextureAssetError{ TextureAssetErrorCode::ImportFailed, Asset, ImportedImage.error().Message });
			PreparedTextureData Prepared;
			Prepared.Width = ImportedImage->Width;
			Prepared.Height = ImportedImage->Height;
			Prepared.Format = Format;
			{
				ScopedDiagnosticTimer Timer(m_Diagnostics, "Texture mip generation", "TextureAssetCache");
			Prepared.MipLevels = Detail::BuildTextureMipChain(ImportedImage->Width, ImportedImage->Height,
				ImportedImage->RGBA8Pixels, Format == TextureFormat::RGBA8_Srgb
					? Detail::ImageColorSpace::Srgb : Detail::ImageColorSpace::Linear);
			}
			TextureDesc Description;
			Description.Width = Prepared.Width;
			Description.Height = Prepared.Height;
			Description.Format = Prepared.Format;
			Description.MipLevels = static_cast<uint32_t>(Prepared.MipLevels.size());
			Description.DebugName = "Image asset " + Asset.ToString();
			std::vector<TextureSubresourceData> InitialData;
			InitialData.reserve(Prepared.MipLevels.size());
			for (uint32_t MipLevel = 0; MipLevel < Prepared.MipLevels.size(); ++MipLevel)
			{
				const OwnedMipLevel& Level = Prepared.MipLevels[MipLevel];
				InitialData.push_back({ MipLevel, 0, std::as_bytes(std::span(Level.Pixels)), 0 });
			}
			auto CreatedTexture = m_Runtime.CreateTexture(Description, InitialData);
			if (!CreatedTexture)
				return std::unexpected(TextureAssetError{ TextureAssetErrorCode::TextureCreationFailed, Asset,
					CreatedTexture.error().Message });
			State.Current = std::move(*CreatedTexture);
			++State.ResourceRevision;
			if (m_Diagnostics)
				(void)m_Diagnostics->RecordEvent(DiagnosticEventKind::Resource, "Texture cache resource created", "TextureAssetCache", "objects", 1);
			return std::cref(*State.Current);
		}

		if (State.SessionKey != m_SessionKey)
		{
			if (m_SessionKey.empty())
			{
				if (State.Current)
					return std::cref(*State.Current);
				return std::unexpected(TextureAssetError{ TextureAssetErrorCode::CacheFailure, Asset,
					"Asynchronous texture loading requires a non-empty project/session identity" });
			}
			State.Pending.reset();
			State.FailureMessage.clear();
			State.DiagnosticSessionId = 0;
			State.DiagnosticTimingsRecorded = false;
			if (m_Diagnostics)
			{
				uint64_t Start = 0;
				(void)m_Diagnostics->BeginDurationSession(State.DiagnosticSessionId, Start);
			}
			State.TerminalFailure = false;
			State.AdmissionDeferred = false;
			State.TerminalFailureCode = TextureAssetErrorCode::TextureCreationFailed;
			auto RegistrySnapshot = m_RegistryOwner;
			const std::filesystem::path ProjectRoot = m_ProjectRoot;
			DerivedDataArtifactStore* ArtifactStore = m_ArtifactStore;
			auto Requested = m_Preparation->Request(
				{ m_SessionKey, "texture-request/" + Asset.ToString() + "/" + std::to_string(static_cast<uint8_t>(Format)) },
				AssetPreparationPriority::Interactive,
				EstimatedTextureWorkingBytes,
				[Asset, Format, ProjectRoot, RegistrySnapshot, ArtifactStore](std::stop_token StopToken)
					-> std::expected<AssetPreparationService::CpuPayload, AssetPreparationError>
				{
					auto Prepared = Detail::PrepareTextureCpuProduct(
						Asset, ProjectRoot, *RegistrySnapshot,
						Format == TextureFormat::RGBA8_Srgb ? Detail::ImageColorSpace::Srgb : Detail::ImageColorSpace::Linear,
						ArtifactStore, StopToken);
					if (!Prepared)
						return std::unexpected(AssetPreparationError{ AssetPreparationErrorCode::ProcessingFailed, Prepared.error() });
					const size_t RetainedBytes = GetPreparedTextureRetainedBytes(Prepared->Product);
					return MakeAssetPreparationValue(std::move(*Prepared), RetainedBytes);
				});
			if (!Requested)
			{
				if (State.Current)
				{
					State.AdmissionDeferred = Requested.error().Code == AssetPreparationErrorCode::WorkingSetLimit ||
						Requested.error().Code == AssetPreparationErrorCode::QueueFull;
					return std::cref(*State.Current);
				}
				if (Requested.error().Code == AssetPreparationErrorCode::WorkingSetLimit ||
					Requested.error().Code == AssetPreparationErrorCode::QueueFull)
				{
					State.AdmissionDeferred = true;
					return std::unexpected(TextureAssetError{ TextureAssetErrorCode::Pending, Asset, Requested.error().Message });
				}
				return std::unexpected(TextureAssetError{ TextureAssetErrorCode::CacheFailure, Asset, Requested.error().Message });
			}
			State.Pending.emplace(std::move(*Requested));
			State.AdmissionDeferred = false;
			State.SessionKey = m_SessionKey;
			++m_CacheMisses;
		}

		if (!State.Pending)
		{
			if (State.Current)
				return std::cref(*State.Current);
			if (State.TerminalFailure)
				return std::unexpected(TextureAssetError{ State.TerminalFailureCode, Asset, State.FailureMessage });
			return std::unexpected(TextureAssetError{ TextureAssetErrorCode::Pending, Asset, "Texture preparation is pending" });
		}
		if (!CollectReadyProduct)
		{
			if (State.Current)
				return std::cref(*State.Current);
			return std::unexpected(TextureAssetError{ TextureAssetErrorCode::Pending, Asset, "Texture preparation is pending" });
		}

		const AssetPreparationSnapshot Prepared = State.Pending->Poll();
		switch (Prepared.Status)
		{
			case AssetPreparationStatus::Queued:
			case AssetPreparationStatus::Processing:
				if (State.Current)
					return std::cref(*State.Current);
				return std::unexpected(TextureAssetError{ TextureAssetErrorCode::Pending, Asset, "Texture preparation is pending" });
			case AssetPreparationStatus::CpuReady:
			{
				if (State.TerminalFailure)
				{
					if (State.Current)
						return std::cref(*State.Current);
					return std::unexpected(TextureAssetError{ State.TerminalFailureCode, Asset,
						State.FailureMessage });
				}
				const auto* TextureProduct = Prepared.CpuPayload
					? dynamic_cast<const AssetPreparationValue<Detail::AssetCpuProductResult<Detail::PreparedTextureCpuProduct>>*>(Prepared.CpuPayload.get()) : nullptr;
				if (!TextureProduct)
				{
					State.TerminalFailure = true;
					State.TerminalFailureCode = TextureAssetErrorCode::CacheFailure;
					State.FailureMessage = "Prepared texture result has an unexpected CPU product type";
					State.Pending.reset();
					if (State.Current)
						return std::cref(*State.Current);
					return std::unexpected(TextureAssetError{ TextureAssetErrorCode::CacheFailure, Asset, State.FailureMessage });
				}
				const auto& CpuResult = TextureProduct->Get();
				const auto& Data = CpuResult.Product;
				if (!State.DiagnosticTimingsRecorded && m_Diagnostics)
				{
					const auto RecordPhase = [this, &State](const Detail::AssetCpuPhaseTiming& Timing, std::string_view Name)
					{
						if (Timing.EndTimestampNanoseconds > Timing.StartTimestampNanoseconds)
							(void)m_Diagnostics->RecordDurationForSession(State.DiagnosticSessionId,
								Timing.StartTimestampNanoseconds, Timing.EndTimestampNanoseconds, Name, "TextureAssetCache");
					};
					RecordPhase(CpuResult.ObservationAndLookup, "Image source read and artifact lookup");
					RecordPhase(CpuResult.Processing, "Image decode and mip processing");
					RecordPhase(CpuResult.CacheWrite, "Image artifact write");
					(void)m_Diagnostics->RecordEvent(DiagnosticEventKind::Counter,
						CpuResult.CacheOutcome == Detail::AssetCpuProductCacheOutcome::ArtifactHit ? "Image artifact cache hit" : "Image artifact cache miss",
						"TextureAssetCache", "products", 1);
					State.DiagnosticTimingsRecorded = true;
				}
				const size_t UploadBytes = Prepared.CpuPayload->GetRetainedBytes();
				if (m_UploadBudget && (UploadBytes == 0 || UploadBytes > AssetUploadBudget::MaximumIndivisibleProductBytes))
				{
					State.TerminalFailure = true;
					State.TerminalFailureCode = TextureAssetErrorCode::TextureCreationFailed;
					State.FailureMessage = "Prepared texture exceeds the bounded indivisible GPU upload size";
					State.Pending.reset();
					if (State.Current) return std::cref(*State.Current);
					return std::unexpected(TextureAssetError{ TextureAssetErrorCode::TextureCreationFailed, Asset, State.FailureMessage });
				}
				if (m_UploadBudget && !m_UploadBudget->TryReserve(UploadBytes))
				{
					if (State.Current) return std::cref(*State.Current);
					return std::unexpected(TextureAssetError{ TextureAssetErrorCode::Pending, Asset,
						"Texture CPU product is ready and waiting for the renderer upload budget" });
				}
				TextureDesc Description;
				Description.Width = Data.Width;
				Description.Height = Data.Height;
				Description.Format = Data.ColorSpace == Detail::ImageColorSpace::Srgb
					? TextureFormat::RGBA8_Srgb : TextureFormat::RGBA8_UNorm;
				Description.MipLevels = static_cast<uint32_t>(Data.MipLevels.size());
				Description.DebugName = "Image asset " + Asset.ToString();
				std::vector<TextureSubresourceData> InitialData;
				InitialData.reserve(Data.MipLevels.size());
				for (uint32_t MipLevel = 0; MipLevel < Data.MipLevels.size(); ++MipLevel)
				{
					const OwnedMipLevel& Level = Data.MipLevels[MipLevel];
					InitialData.push_back({ MipLevel, 0, std::as_bytes(std::span(Level.Pixels)), 0 });
				}
				auto CreatedTexture = [&]()
				{
					ScopedDiagnosticTimer Timer(m_Diagnostics, "Texture GPU resource creation", "TextureAssetCache");
					return m_Runtime.CreateTexture(Description, InitialData);
				}();
				if (!CreatedTexture)
				{
					State.TerminalFailure = true;
					State.TerminalFailureCode = TextureAssetErrorCode::TextureCreationFailed;
					State.FailureMessage = CreatedTexture.error().Message;
					State.Pending.reset();
					if (State.Current)
						return std::cref(*State.Current);
					return std::unexpected(TextureAssetError{ TextureAssetErrorCode::TextureCreationFailed, Asset,
						State.FailureMessage });
				}
				State.Current = std::move(*CreatedTexture);
				++State.ResourceRevision;
				State.Pending.reset();
				State.TerminalFailure = false;
				State.TerminalFailureCode = TextureAssetErrorCode::TextureCreationFailed;
				State.FailureMessage.clear();
				if (m_Diagnostics)
					(void)m_Diagnostics->RecordEvent(DiagnosticEventKind::Resource, "Texture cache resource created", "TextureAssetCache", "objects", 1);
				return std::cref(*State.Current);
			}
			case AssetPreparationStatus::Failed:
				State.FailureMessage = Prepared.Error ? Prepared.Error->Message : "Texture CPU preparation failed";
				State.TerminalFailure = true;
				State.TerminalFailureCode = TextureAssetErrorCode::ImportFailed;
				State.Pending.reset();
				if (State.Current)
					return std::cref(*State.Current);
				return std::unexpected(TextureAssetError{ TextureAssetErrorCode::ImportFailed, Asset, State.FailureMessage });
			case AssetPreparationStatus::Cancelled:
				State.Pending.reset();
				State.SessionKey.clear();
				State.TerminalFailure = false;
				if (State.Current)
					return std::cref(*State.Current);
				return std::unexpected(TextureAssetError{ TextureAssetErrorCode::Pending, Asset, "Texture preparation was cancelled" });
		}
		return std::unexpected(TextureAssetError{ TextureAssetErrorCode::CacheFailure, Asset,
			"Unrecognized texture preparation state" });
#endif
	}

	bool TextureAssetCache::AdvancePending()
	{
#if defined(PF_COOKED_RUNTIME_ONLY)
		return false;
#else
		std::vector<CacheKey> Keys;
		Keys.reserve(m_Textures.size());
		for (const auto& [Key, State] : m_Textures)
		{
			(void)State;
			Keys.push_back(Key);
		}

		bool PublishedReplacement = false;
		for (const CacheKey& Key : Keys)
		{
			const auto Found = m_Textures.find(Key);
			if (Found == m_Textures.end())
				continue;
			const uint64_t PreviousRevision = Found->second.ResourceRevision;
			(void)GetOrLoadInternal(Key.Asset, Key.Format, true);
			const auto Updated = m_Textures.find(Key);
			PublishedReplacement |= Updated != m_Textures.end() && Updated->second.ResourceRevision != PreviousRevision;
		}
		return PublishedReplacement;
#endif
	}

	uint64_t TextureAssetCache::GetResourceRevision(const AssetID& Asset, TextureFormat Format) const noexcept
	{
		const auto Found = m_Textures.find(CacheKey{ Asset, Format });
		return Found == m_Textures.end() ? 0 : Found->second.ResourceRevision;
	}

	bool TextureAssetCache::HasPendingWork() const noexcept
	{
#if defined(PF_COOKED_RUNTIME_ONLY)
		return false;
#else
		return std::ranges::any_of(m_Textures, [this](const auto& Pair)
		{
			const auto& State = Pair.second;
			return State.Pending.has_value() || State.AdmissionDeferred;
		});
#endif
	}

	void TextureAssetCache::Clear() noexcept
	{
		const uint64_t RetiredCount = GetLoadedCount();
		m_Textures.clear();
		if (m_Diagnostics && RetiredCount > 0)
			(void)m_Diagnostics->RecordEvent(DiagnosticEventKind::Resource, "Texture cache resources retired", "TextureAssetCache", "objects", RetiredCount);
		if (m_Diagnostics && m_CacheHits > 0)
			(void)m_Diagnostics->RecordEvent(DiagnosticEventKind::Counter, "Texture cache hits", "TextureAssetCache", "lookups", m_CacheHits);
		if (m_Diagnostics && m_CacheMisses > 0)
			(void)m_Diagnostics->RecordEvent(DiagnosticEventKind::Counter, "Texture cache misses", "TextureAssetCache", "lookups", m_CacheMisses);
		m_CacheHits = 0;
		m_CacheMisses = 0;
	}

	size_t TextureAssetCache::GetLoadedCount() const noexcept
	{
		size_t Count = 0;
		for (const auto& [Key, Entry] : m_Textures)
		{
			(void)Key;
			Count += Entry.Current ? 1 : 0;
		}
		return Count;
	}

	size_t TextureAssetCache::CacheKeyHash::operator()(const CacheKey& Key) const noexcept
	{
		const size_t AssetHash = UUIDHash{}(Key.Asset);
		const size_t FormatHash = std::hash<uint8_t>{}(static_cast<uint8_t>(Key.Format));
		return AssetHash ^ (FormatHash + static_cast<size_t>(0x9e3779b9) + (AssetHash << 6) + (AssetHash >> 2));
	}
} // namespace PulseForge
