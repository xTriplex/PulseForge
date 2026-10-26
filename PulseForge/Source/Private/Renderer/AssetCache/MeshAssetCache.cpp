#include "Renderer/AssetCache/MeshAssetCache.h"

#include "Assets/DerivedDataArtifactStore.h"
#include "Assets/GltfMeshImporter.h"
#include "Core/Diagnostics.h"
#include "Assets/Detail/AssetCpuProducts.h"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

namespace PulseForge
{
	namespace
	{
		// GltfMeshImporter permits up to 10M vertices/30M indices; this reservation is a bounded admission estimate,
		// not a precise peak allocator measurement. Products beyond service completion limits fail explicitly.
		constexpr size_t EstimatedMeshWorkingBytes = 512ull * 1024ull * 1024ull;

		size_t GetImportedMeshRetainedBytes(const ImportedGltfMesh& Mesh) noexcept
		{
			constexpr size_t Maximum = (std::numeric_limits<size_t>::max)();
			const size_t VertexBytes = Mesh.Vertices.size() > Maximum / sizeof(GltfMeshVertex)
				? Maximum : Mesh.Vertices.size() * sizeof(GltfMeshVertex);
			const size_t IndexBytes = Mesh.Indices.size() > Maximum / sizeof(uint32_t)
				? Maximum : Mesh.Indices.size() * sizeof(uint32_t);
			return VertexBytes > Maximum - IndexBytes ? Maximum : VertexBytes + IndexBytes;
		}

		std::string MakeMeshRecipeKey(const AssetID& Asset, std::string_view CanonicalSettings)
		{
			return "gltf-static-mesh-v1/" + Asset.ToString() + "/mesh-0/primitive-0/" + std::string(CanonicalSettings);
		}

	}

	MeshAssetCache::MeshAssetCache(
		RenderDevice& Runtime,
		std::filesystem::path ProjectRoot,
		const AssetRegistry& Registry,
		DiagnosticRecorder* Diagnostics)
		: m_Runtime(Runtime), m_ProjectRoot(std::move(ProjectRoot)),
		  m_RegistryOwner(std::make_shared<const AssetRegistry>(Registry)), m_Registry(m_RegistryOwner.get()), m_Diagnostics(Diagnostics)
	{
	}

	MeshAssetCache::MeshAssetCache(
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

	MeshAssetCache::MeshAssetCache(
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

	MeshAssetCache::~MeshAssetCache()
	{
		Clear();
	}

	void MeshAssetCache::SetSessionKey(std::string SessionKey)
	{
		if (SessionKey == m_SessionKey)
			return;
		m_SessionKey = std::move(SessionKey);
		for (auto& [Asset, Entry] : m_Meshes)
		{
			(void)Asset;
			Entry.Pending.reset();
			Entry.SessionKey.clear();
			Entry.FailureMessage.clear();
			Entry.TerminalFailure = false;
			Entry.TerminalFailureCode = MeshAssetErrorCode::MeshCreationFailed;
		}
	}

	void MeshAssetCache::SetRegistrySnapshot(std::shared_ptr<const AssetRegistry> Registry) noexcept
	{
		if (!Registry)
			return;
		m_RegistryOwner = std::move(Registry);
		m_Registry = m_RegistryOwner.get();
	}

	std::expected<std::reference_wrapper<const Mesh>, MeshAssetError> MeshAssetCache::GetOrLoad(const AssetID& Asset)
	{
		return GetOrLoadInternal(Asset, false);
	}

	std::expected<std::reference_wrapper<const Mesh>, MeshAssetError> MeshAssetCache::GetOrLoadInternal(
		const AssetID& Asset, bool CollectReadyProduct)
	{
		if (Asset.IsNil())
			return std::unexpected(MeshAssetError{ MeshAssetErrorCode::AssetNotFound, Asset,
				"Cannot load a mesh with a nil asset UUID" });
		const auto Record = m_Registry->Find(Asset);
		if (!Record)
			return std::unexpected(MeshAssetError{ MeshAssetErrorCode::AssetNotFound, Asset,
				"Mesh asset UUID " + Asset.ToString() + " is not present in the project asset registry" });

		auto [EntryIterator, WasInserted] = m_Meshes.try_emplace(Asset);
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
			auto ImportedMesh = [&]()
			{
				ScopedDiagnosticTimer Timer(m_Diagnostics, "Mesh asset import", "MeshAssetCache");
				return GltfMeshImporter::ImportStaticPrimitive(Asset, m_ProjectRoot, *m_Registry);
			}();
			if (!ImportedMesh)
				return std::unexpected(MeshAssetError{ MeshAssetErrorCode::ImportFailed, Asset, ImportedMesh.error().Message });
			const MeshDesc Description = ImportedMesh->GetMeshDescription();
			auto CreatedMesh = [&]()
			{
				ScopedDiagnosticTimer Timer(m_Diagnostics, "Mesh GPU resource creation", "MeshAssetCache");
				return m_Runtime.CreateMesh(Description);
			}();
			if (!CreatedMesh)
				return std::unexpected(MeshAssetError{ MeshAssetErrorCode::MeshCreationFailed, Asset, CreatedMesh.error().Message });
			try
			{
				State.Current = std::move(CreatedMesh.value());
				if (m_Diagnostics)
					(void)m_Diagnostics->RecordEvent(DiagnosticEventKind::Resource, "Mesh cache resource created", "MeshAssetCache", "objects", 1);
				return std::cref(*State.Current);
			}
			catch (const std::exception& Exception)
			{
				return std::unexpected(MeshAssetError{ MeshAssetErrorCode::CacheFailure, Asset,
					std::string("Could not retain the uploaded mesh resource: ") + Exception.what() });
			}
		}

		if (State.SessionKey != m_SessionKey)
		{
			if (m_SessionKey.empty())
			{
				if (State.Current)
					return std::cref(*State.Current);
				return std::unexpected(MeshAssetError{ MeshAssetErrorCode::CacheFailure, Asset,
					"Asynchronous mesh loading requires a non-empty project/session identity" });
			}
			State.Pending.reset();
			State.FailureMessage.clear();
			State.DiagnosticSessionId = 0;
			State.DiagnosticTimingsRecorded = false;
			State.TerminalFailureCode = MeshAssetErrorCode::MeshCreationFailed;
			if (m_Diagnostics)
			{
				uint64_t Start = 0;
				(void)m_Diagnostics->BeginDurationSession(State.DiagnosticSessionId, Start);
			}
			State.TerminalFailure = false;
			State.AdmissionDeferred = false;
			const std::filesystem::path ProjectRoot = m_ProjectRoot;
			DerivedDataArtifactStore* ArtifactStore = m_ArtifactStore;
			const auto RegistrySnapshot = m_RegistryOwner;
			const std::string Session = m_SessionKey;
			const std::string Recipe = MakeMeshRecipeKey(Asset, "authored-settings");
			auto Requested = m_Preparation->Request(
				{ Session, Recipe }, AssetPreparationPriority::Interactive, EstimatedMeshWorkingBytes,
				[Asset, ProjectRoot, RegistrySnapshot, ArtifactStore](std::stop_token StopToken)
					-> std::expected<AssetPreparationService::CpuPayload, AssetPreparationError>
				{
					const auto Record = RegistrySnapshot->Find(Asset);
					if (!Record)
						return std::unexpected(AssetPreparationError{ AssetPreparationErrorCode::ProcessingFailed,
							"Mesh asset was removed from the retained project registry" });
					const auto Metadata = AssetMetadataSerializer::LoadFromFile(
						AssetMetadataSerializer::GetSidecarPath(ProjectRoot / Record->ProjectRelativePath));
					if (!Metadata || Metadata->ID != Asset)
						return std::unexpected(AssetPreparationError{ AssetPreparationErrorCode::ProcessingFailed,
							Metadata ? "Mesh sidecar identity no longer matches the asset registry" : Metadata.error().Message });
					auto Settings = GltfMeshImportSettings::Parse(Metadata->ImportSettingsJson);
					if (!Settings)
						return std::unexpected(AssetPreparationError{ AssetPreparationErrorCode::ProcessingFailed, Settings.error() });
					auto EffectiveSettings = Settings->Canonicalize();
					if (!EffectiveSettings)
						return std::unexpected(AssetPreparationError{ AssetPreparationErrorCode::ProcessingFailed, EffectiveSettings.error() });
					auto Prepared = Detail::PrepareMeshCpuProduct(
						Asset, ProjectRoot, *RegistrySnapshot, *EffectiveSettings, ArtifactStore, StopToken);
					if (!Prepared)
						return std::unexpected(AssetPreparationError{ AssetPreparationErrorCode::ProcessingFailed, Prepared.error() });
					const size_t RetainedBytes = GetImportedMeshRetainedBytes(Prepared->Product);
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
					return std::unexpected(MeshAssetError{ MeshAssetErrorCode::Pending, Asset, Requested.error().Message });
				}
				return std::unexpected(MeshAssetError{ MeshAssetErrorCode::CacheFailure, Asset, Requested.error().Message });
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
				return std::unexpected(MeshAssetError{ State.TerminalFailureCode, Asset, State.FailureMessage });
			return std::unexpected(MeshAssetError{ MeshAssetErrorCode::Pending, Asset, "Mesh preparation is pending" });
		}
		if (!CollectReadyProduct)
		{
			if (State.Current)
				return std::cref(*State.Current);
			return std::unexpected(MeshAssetError{ MeshAssetErrorCode::Pending, Asset, "Mesh preparation is pending" });
		}

		const AssetPreparationSnapshot Prepared = State.Pending->Poll();
		switch (Prepared.Status)
		{
			case AssetPreparationStatus::Queued:
			case AssetPreparationStatus::Processing:
				if (State.Current)
					return std::cref(*State.Current);
				return std::unexpected(MeshAssetError{ MeshAssetErrorCode::Pending, Asset, "Mesh preparation is pending" });
			case AssetPreparationStatus::CpuReady:
			{
				if (State.TerminalFailure)
				{
					if (State.Current)
						return std::cref(*State.Current);
					return std::unexpected(MeshAssetError{ MeshAssetErrorCode::MeshCreationFailed, Asset, State.FailureMessage });
				}
				const auto* MeshProduct = Prepared.CpuPayload
					? dynamic_cast<const AssetPreparationValue<Detail::AssetCpuProductResult<ImportedGltfMesh>>*>(Prepared.CpuPayload.get()) : nullptr;
				if (!MeshProduct)
				{
					State.TerminalFailure = true;
					State.TerminalFailureCode = MeshAssetErrorCode::CacheFailure;
					State.FailureMessage = "Prepared mesh result has an unexpected CPU product type";
					State.Pending.reset();
					if (State.Current)
						return std::cref(*State.Current);
					return std::unexpected(MeshAssetError{ MeshAssetErrorCode::CacheFailure, Asset, State.FailureMessage });
				}
				const auto& CpuResult = MeshProduct->Get();
				if (!State.DiagnosticTimingsRecorded && m_Diagnostics)
				{
					const auto RecordPhase = [this, &State](const Detail::AssetCpuPhaseTiming& Timing, std::string_view Name)
					{
						if (Timing.EndTimestampNanoseconds > Timing.StartTimestampNanoseconds)
							(void)m_Diagnostics->RecordDurationForSession(State.DiagnosticSessionId,
								Timing.StartTimestampNanoseconds, Timing.EndTimestampNanoseconds, Name, "MeshAssetCache");
					};
					RecordPhase(CpuResult.ObservationAndLookup, "Mesh source inspection and artifact lookup");
					RecordPhase(CpuResult.Processing, "Mesh CPU import");
					RecordPhase(CpuResult.CacheWrite, "Mesh artifact write");
					(void)m_Diagnostics->RecordEvent(DiagnosticEventKind::Counter,
						CpuResult.CacheOutcome == Detail::AssetCpuProductCacheOutcome::ArtifactHit ? "Mesh artifact cache hit" : "Mesh artifact cache miss",
						"MeshAssetCache", "products", 1);
					State.DiagnosticTimingsRecorded = true;
				}
				const size_t UploadBytes = Prepared.CpuPayload->GetRetainedBytes();
				if (m_UploadBudget && (UploadBytes == 0 || UploadBytes > AssetUploadBudget::MaximumIndivisibleProductBytes))
				{
					State.TerminalFailure = true;
					State.TerminalFailureCode = MeshAssetErrorCode::MeshCreationFailed;
					State.FailureMessage = "Prepared mesh exceeds the bounded indivisible GPU upload size";
					State.Pending.reset();
					if (State.Current) return std::cref(*State.Current);
					return std::unexpected(MeshAssetError{ MeshAssetErrorCode::MeshCreationFailed, Asset, State.FailureMessage });
				}
				if (m_UploadBudget && !m_UploadBudget->TryReserve(UploadBytes))
				{
					if (State.Current) return std::cref(*State.Current);
					return std::unexpected(MeshAssetError{ MeshAssetErrorCode::Pending, Asset,
						"Mesh CPU product is ready and waiting for the renderer upload budget" });
				}
				const MeshDesc Description = CpuResult.Product.GetMeshDescription();
				auto CreatedMesh = [&]()
				{
					ScopedDiagnosticTimer Timer(m_Diagnostics, "Mesh GPU resource creation", "MeshAssetCache");
					return m_Runtime.CreateMesh(Description);
				}();
				if (!CreatedMesh)
				{
					State.TerminalFailure = true;
					State.TerminalFailureCode = MeshAssetErrorCode::MeshCreationFailed;
					State.FailureMessage = CreatedMesh.error().Message;
					State.Pending.reset();
					if (State.Current)
						return std::cref(*State.Current);
					return std::unexpected(MeshAssetError{ MeshAssetErrorCode::MeshCreationFailed, Asset, State.FailureMessage });
				}
				State.Current = std::move(*CreatedMesh);
				State.Pending.reset();
				State.TerminalFailure = false;
				State.TerminalFailureCode = MeshAssetErrorCode::MeshCreationFailed;
				State.FailureMessage.clear();
				if (m_Diagnostics)
					(void)m_Diagnostics->RecordEvent(DiagnosticEventKind::Resource, "Mesh cache resource created", "MeshAssetCache", "objects", 1);
				return std::cref(*State.Current);
			}
			case AssetPreparationStatus::Failed:
				State.FailureMessage = Prepared.Error ? Prepared.Error->Message : "Mesh CPU preparation failed";
				State.TerminalFailure = true;
				State.TerminalFailureCode = MeshAssetErrorCode::ImportFailed;
				State.Pending.reset();
				if (State.Current)
					return std::cref(*State.Current);
				return std::unexpected(MeshAssetError{ MeshAssetErrorCode::ImportFailed, Asset, State.FailureMessage });
			case AssetPreparationStatus::Cancelled:
				State.Pending.reset();
				State.SessionKey.clear();
				if (State.Current)
					return std::cref(*State.Current);
				return std::unexpected(MeshAssetError{ MeshAssetErrorCode::Pending, Asset, "Mesh preparation was cancelled" });
		}
		return std::unexpected(MeshAssetError{ MeshAssetErrorCode::CacheFailure, Asset, "Unrecognized mesh preparation state" });
	}

	bool MeshAssetCache::AdvancePending()
	{
		std::vector<AssetID> Assets;
		Assets.reserve(m_Meshes.size());
		for (const auto& [Asset, State] : m_Meshes)
		{
			(void)State;
			Assets.push_back(Asset);
		}

		bool PublishedReplacement = false;
		for (const AssetID& Asset : Assets)
		{
			const auto Found = m_Meshes.find(Asset);
			if (Found == m_Meshes.end())
				continue;
			const Mesh* Previous = Found->second.Current.get();
			(void)GetOrLoadInternal(Asset, true);
			const auto Updated = m_Meshes.find(Asset);
			PublishedReplacement |= Updated != m_Meshes.end() && Updated->second.Current.get() != Previous;
		}
		return PublishedReplacement;
	}

	void MeshAssetCache::Clear() noexcept
	{
		const uint64_t RetiredCount = GetLoadedCount();
		m_Meshes.clear();
		if (m_Diagnostics && RetiredCount > 0)
			(void)m_Diagnostics->RecordEvent(DiagnosticEventKind::Resource, "Mesh cache resources retired", "MeshAssetCache", "objects", RetiredCount);
		if (m_Diagnostics && m_CacheHits > 0)
			(void)m_Diagnostics->RecordEvent(DiagnosticEventKind::Counter, "Mesh cache hits", "MeshAssetCache", "lookups", m_CacheHits);
		if (m_Diagnostics && m_CacheMisses > 0)
			(void)m_Diagnostics->RecordEvent(DiagnosticEventKind::Counter, "Mesh cache misses", "MeshAssetCache", "lookups", m_CacheMisses);
		m_CacheHits = 0;
		m_CacheMisses = 0;
	}

	size_t MeshAssetCache::GetLoadedCount() const noexcept
	{
		size_t Count = 0;
		for (const auto& [Asset, Entry] : m_Meshes)
		{
			(void)Asset;
			Count += Entry.Current ? 1 : 0;
		}
		return Count;
	}

	bool MeshAssetCache::HasPendingWork() const noexcept
	{
		return std::ranges::any_of(m_Meshes, [this](const auto& Pair)
		{
			const auto& State = Pair.second;
			return State.Pending.has_value() || State.AdmissionDeferred;
		});
	}
} // namespace PulseForge
