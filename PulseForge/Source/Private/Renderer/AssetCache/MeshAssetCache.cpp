#include "Renderer/AssetCache/MeshAssetCache.h"

#include "Assets/AssetDependencyExtractor.h"
#include "Assets/AssetImportProvenance.h"
#include "Assets/DerivedDataArtifactStore.h"
#include "Assets/GltfMeshImporter.h"
#include "Core/Diagnostics.h"
#include "Assets/Detail/AssetCpuProducts.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <exception>
#include <limits>
#include <optional>
#include <span>
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
		constexpr uint32_t MeshArtifactSchemaVersion = 1;
		constexpr uint32_t MaximumMeshArtifactVertices = 4'000'000;
		constexpr uint32_t MaximumMeshArtifactIndices = 12'000'000;
		constexpr size_t MaximumMeshArtifactBytes = 256ull * 1024ull * 1024ull;

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

		void AppendU32(std::vector<std::byte>& Output, uint32_t Value)
		{
			for (unsigned Shift = 0; Shift < 32; Shift += 8)
				Output.push_back(static_cast<std::byte>((Value >> Shift) & 0xffu));
		}

		bool ReadU32(std::span<const std::byte> Input, size_t& Offset, uint32_t& Value)
		{
			if (Offset > Input.size() || Input.size() - Offset < 4)
				return false;
			Value = 0;
			for (unsigned Index = 0; Index < 4; ++Index)
				Value |= static_cast<uint32_t>(std::to_integer<uint8_t>(Input[Offset++])) << (Index * 8);
			return true;
		}

		void AppendFloat(std::vector<std::byte>& Output, float Value)
		{
			AppendU32(Output, std::bit_cast<uint32_t>(Value));
		}

		bool ReadFloat(std::span<const std::byte> Input, size_t& Offset, float& Value)
		{
			uint32_t Bits = 0;
			if (!ReadU32(Input, Offset, Bits))
				return false;
			Value = std::bit_cast<float>(Bits);
			return std::isfinite(Value);
		}

		std::expected<std::vector<std::byte>, std::string> EncodeMeshArtifact(const ImportedGltfMesh& Mesh)
		{
			if (Mesh.Name.size() > 4096 || Mesh.Vertices.size() > MaximumMeshArtifactVertices ||
				Mesh.Indices.size() > MaximumMeshArtifactIndices)
				return std::unexpected("Mesh product exceeds the artifact codec bounds");
			constexpr size_t FloatCountPerVertex = 16;
			if (Mesh.Vertices.size() > (MaximumMeshArtifactBytes - 16) / (FloatCountPerVertex * sizeof(uint32_t)))
				return std::unexpected("Mesh vertex payload exceeds the artifact codec size bound");
			std::vector<std::byte> Output;
			Output.reserve(16 + Mesh.Name.size() + Mesh.Vertices.size() * FloatCountPerVertex * sizeof(uint32_t) +
				Mesh.Indices.size() * sizeof(uint32_t));
			Output.insert(Output.end(), { std::byte{'P'}, std::byte{'F'}, std::byte{'M'}, std::byte{'S'} });
			AppendU32(Output, MeshArtifactSchemaVersion);
			AppendU32(Output, static_cast<uint32_t>(Mesh.Name.size()));
			for (const unsigned char Character : Mesh.Name)
				Output.push_back(static_cast<std::byte>(Character));
			AppendU32(Output, static_cast<uint32_t>(Mesh.Vertices.size()));
			AppendU32(Output, static_cast<uint32_t>(Mesh.Indices.size()));
			for (const GltfMeshVertex& Vertex : Mesh.Vertices)
			{
				for (float Value : Vertex.Position) AppendFloat(Output, Value);
				for (float Value : Vertex.Color) AppendFloat(Output, Value);
				for (float Value : Vertex.TexCoord) AppendFloat(Output, Value);
				for (float Value : Vertex.Normal) AppendFloat(Output, Value);
				for (float Value : Vertex.Tangent) AppendFloat(Output, Value);
			}
			for (const uint32_t Index : Mesh.Indices)
				AppendU32(Output, Index);
			if (Output.size() > MaximumMeshArtifactBytes)
				return std::unexpected("Encoded mesh product exceeds the artifact size bound");
			return Output;
		}

		std::expected<ImportedGltfMesh, std::string> DecodeMeshArtifact(std::span<const std::byte> Input)
		{
			if (Input.size() < 16 || Input.size() > MaximumMeshArtifactBytes ||
				Input[0] != std::byte{'P'} || Input[1] != std::byte{'F'} || Input[2] != std::byte{'M'} || Input[3] != std::byte{'S'})
				return std::unexpected("Mesh artifact has an invalid magic or size");
			size_t Offset = 4;
			uint32_t Version = 0, NameLength = 0;
			if (!ReadU32(Input, Offset, Version) || Version != MeshArtifactSchemaVersion ||
				!ReadU32(Input, Offset, NameLength) || NameLength > 4096 || NameLength > Input.size() - Offset)
				return std::unexpected("Mesh artifact has an unsupported schema or invalid name length");
			ImportedGltfMesh Mesh;
			Mesh.Name.assign(reinterpret_cast<const char*>(Input.data() + Offset), NameLength);
			Offset += NameLength;
			uint32_t VertexCount = 0, IndexCount = 0;
			if (!ReadU32(Input, Offset, VertexCount) || !ReadU32(Input, Offset, IndexCount) ||
				VertexCount == 0 || VertexCount > MaximumMeshArtifactVertices || IndexCount == 0 ||
				IndexCount > MaximumMeshArtifactIndices || IndexCount % 3 != 0)
				return std::unexpected("Mesh artifact has invalid geometry counts");
			const uint64_t Required = static_cast<uint64_t>(VertexCount) * 16ull * 4ull + static_cast<uint64_t>(IndexCount) * 4ull;
			if (Required != Input.size() - Offset)
				return std::unexpected("Mesh artifact payload length does not match its geometry counts");
			Mesh.Vertices.resize(VertexCount);
			for (GltfMeshVertex& Vertex : Mesh.Vertices)
			{
				for (float& Value : Vertex.Position) if (!ReadFloat(Input, Offset, Value)) return std::unexpected("Mesh artifact has a non-finite position");
				for (float& Value : Vertex.Color) if (!ReadFloat(Input, Offset, Value)) return std::unexpected("Mesh artifact has a non-finite color");
				for (float& Value : Vertex.TexCoord) if (!ReadFloat(Input, Offset, Value)) return std::unexpected("Mesh artifact has a non-finite texture coordinate");
				for (float& Value : Vertex.Normal) if (!ReadFloat(Input, Offset, Value)) return std::unexpected("Mesh artifact has a non-finite normal");
				for (float& Value : Vertex.Tangent) if (!ReadFloat(Input, Offset, Value)) return std::unexpected("Mesh artifact has a non-finite tangent");
			}
			Mesh.Indices.resize(IndexCount);
			for (uint32_t& Index : Mesh.Indices)
				if (!ReadU32(Input, Offset, Index) || Index >= VertexCount)
					return std::unexpected("Mesh artifact contains an out-of-range index");
			return Mesh;
		}

		std::optional<DerivedDataArtifactKey> MakeMeshArtifactKey(
			const AssetImportRecipe& Recipe,
			const AssetID& Asset,
			std::string_view EffectiveSettings)
		{
			if (Recipe.SourceAsset != Asset || Recipe.SubresourceKey != "mesh/0/primitive/0" ||
				Recipe.ImporterID != "PulseForge.StaticGltfMeshImporter" || Recipe.ImporterVersion != "1" ||
				Recipe.OutputSchemaVersion != 1 || Recipe.TargetID != "cpu-generic-v1" ||
				Recipe.EffectiveSettings != EffectiveSettings)
				return std::nullopt;
			auto Calculated = ComputeAssetImportInputFingerprint(Recipe);
			if (!Calculated || *Calculated != Recipe.InputFingerprint)
				return std::nullopt;
			return DerivedDataArtifactKey{ "gltf-static-mesh", *Calculated, MeshArtifactSchemaVersion };
		}

		AssetImportRecipe MakeRecipe(const AssetImportObservation& Observation)
		{
			AssetImportRecipe Recipe;
			Recipe.SourceAsset = Observation.Asset;
			Recipe.SubresourceKey = Observation.SubresourceKey;
			Recipe.ImporterID = Observation.ImporterID;
			Recipe.ImporterVersion = Observation.ImporterVersion;
			Recipe.OutputSchemaVersion = Observation.OutputSchemaVersion;
			Recipe.EffectiveSettings = Observation.EffectiveSettings;
			Recipe.TargetID = Observation.TargetID;
			Recipe.Inputs = Observation.Inputs;
			Recipe.InputFingerprint = Observation.InputFingerprint;
			return Recipe;
		}

		bool SameRecipe(const AssetImportRecipe& First, const AssetImportRecipe& Second)
		{
			return First.SourceAsset == Second.SourceAsset && First.SubresourceKey == Second.SubresourceKey &&
				First.ImporterID == Second.ImporterID && First.ImporterVersion == Second.ImporterVersion &&
				First.OutputSchemaVersion == Second.OutputSchemaVersion && First.EffectiveSettings == Second.EffectiveSettings &&
				First.TargetID == Second.TargetID && First.Inputs == Second.Inputs &&
				First.InputFingerprint == Second.InputFingerprint;
		}
	}

	MeshAssetCache::MeshAssetCache(
		RenderDevice& Runtime,
		std::filesystem::path ProjectRoot,
		const AssetRegistry& Registry,
		DiagnosticRecorder* Diagnostics)
		: m_Runtime(Runtime), m_ProjectRoot(std::move(ProjectRoot)), m_Registry(Registry), m_Diagnostics(Diagnostics)
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
		: m_Runtime(Runtime), m_ProjectRoot(std::move(ProjectRoot)), m_Registry(Registry),
		  m_Preparation(&Preparation), m_UploadBudget(UploadBudget), m_ArtifactStore(&ArtifactStore),
		  m_SessionKey(std::move(SessionKey)), m_Diagnostics(Diagnostics)
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
			Entry.UploadFailed = false;
		}
	}

	std::expected<std::reference_wrapper<const Mesh>, MeshAssetError> MeshAssetCache::GetOrLoad(const AssetID& Asset)
	{
		if (Asset.IsNil())
			return std::unexpected(MeshAssetError{ MeshAssetErrorCode::AssetNotFound, Asset,
				"Cannot load a mesh with a nil asset UUID" });
		const auto Record = m_Registry.Find(Asset);
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
				return GltfMeshImporter::ImportStaticPrimitive(Asset, m_ProjectRoot, m_Registry);
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
			if (m_Diagnostics)
			{
				uint64_t Start = 0;
				(void)m_Diagnostics->BeginDurationSession(State.DiagnosticSessionId, Start);
			}
			State.UploadFailed = false;
			const std::filesystem::path SourcePath = m_ProjectRoot / Record->ProjectRelativePath;
			const auto Metadata = AssetMetadataSerializer::LoadFromFile(AssetMetadataSerializer::GetSidecarPath(SourcePath));
			if (!Metadata || Metadata->ID != Asset)
			{
				const std::string Message = Metadata
					? "Mesh sidecar identity no longer matches the asset registry"
					: Metadata.error().Message;
				if (State.Current)
					return std::cref(*State.Current);
				return std::unexpected(MeshAssetError{ MeshAssetErrorCode::ImportFailed, Asset, Message });
			}
			auto Settings = GltfMeshImportSettings::Parse(Metadata->ImportSettingsJson);
			if (!Settings)
			{
				if (State.Current)
					return std::cref(*State.Current);
				return std::unexpected(MeshAssetError{ MeshAssetErrorCode::ImportFailed, Asset, Settings.error() });
			}
			auto CanonicalSettings = Settings->Canonicalize();
			if (!CanonicalSettings)
			{
				if (State.Current)
					return std::cref(*State.Current);
				return std::unexpected(MeshAssetError{ MeshAssetErrorCode::ImportFailed, Asset, CanonicalSettings.error() });
			}

			AssetRegistry RegistrySnapshot = m_Registry;
			const std::filesystem::path ProjectRoot = m_ProjectRoot;
			DerivedDataArtifactStore* ArtifactStore = m_ArtifactStore;
			const std::string EffectiveSettings = *CanonicalSettings;
			const std::string Session = m_SessionKey;
			const std::string Recipe = MakeMeshRecipeKey(Asset, EffectiveSettings);
			auto Requested = m_Preparation->Request(
				{ Session, Recipe }, AssetPreparationPriority::Interactive, EstimatedMeshWorkingBytes,
				[Asset, ProjectRoot, RegistrySnapshot = std::move(RegistrySnapshot), EffectiveSettings, ArtifactStore](std::stop_token StopToken) mutable
					-> std::expected<AssetPreparationService::CpuPayload, AssetPreparationError>
				{
					auto Prepared = Detail::PrepareMeshCpuProduct(
						Asset, ProjectRoot, RegistrySnapshot, EffectiveSettings, ArtifactStore, StopToken);
					if (!Prepared)
						return std::unexpected(AssetPreparationError{ AssetPreparationErrorCode::ProcessingFailed, Prepared.error() });
					const size_t RetainedBytes = GetImportedMeshRetainedBytes(Prepared->Product);
					return MakeAssetPreparationValue(std::move(*Prepared), RetainedBytes);
				});
			if (!Requested)
			{
				if (State.Current)
					return std::cref(*State.Current);
				if (Requested.error().Code == AssetPreparationErrorCode::WorkingSetLimit ||
					Requested.error().Code == AssetPreparationErrorCode::QueueFull)
					return std::unexpected(MeshAssetError{ MeshAssetErrorCode::Pending, Asset, Requested.error().Message });
				return std::unexpected(MeshAssetError{ MeshAssetErrorCode::CacheFailure, Asset, Requested.error().Message });
			}
			State.Pending.emplace(std::move(*Requested));
			State.SessionKey = m_SessionKey;
			++m_CacheMisses;
		}

		if (!State.Pending)
		{
			if (State.Current)
				return std::cref(*State.Current);
			if (State.UploadFailed)
				return std::unexpected(MeshAssetError{ MeshAssetErrorCode::MeshCreationFailed, Asset, State.FailureMessage });
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
				if (State.UploadFailed)
				{
					if (State.Current)
						return std::cref(*State.Current);
					return std::unexpected(MeshAssetError{ MeshAssetErrorCode::MeshCreationFailed, Asset, State.FailureMessage });
				}
				const auto* MeshProduct = Prepared.CpuPayload
					? dynamic_cast<const AssetPreparationValue<Detail::AssetCpuProductResult<ImportedGltfMesh>>*>(Prepared.CpuPayload.get()) : nullptr;
				if (!MeshProduct)
				{
					State.UploadFailed = true;
					State.FailureMessage = "Prepared mesh result has an unexpected CPU product type";
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
					State.UploadFailed = true;
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
					State.UploadFailed = true;
					State.FailureMessage = CreatedMesh.error().Message;
					if (State.Current)
						return std::cref(*State.Current);
					return std::unexpected(MeshAssetError{ MeshAssetErrorCode::MeshCreationFailed, Asset, State.FailureMessage });
				}
				State.Current = std::move(*CreatedMesh);
				State.Pending.reset();
				State.UploadFailed = false;
				State.FailureMessage.clear();
				if (m_Diagnostics)
					(void)m_Diagnostics->RecordEvent(DiagnosticEventKind::Resource, "Mesh cache resource created", "MeshAssetCache", "objects", 1);
				return std::cref(*State.Current);
			}
			case AssetPreparationStatus::Failed:
				State.FailureMessage = Prepared.Error ? Prepared.Error->Message : "Mesh CPU preparation failed";
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
} // namespace PulseForge
