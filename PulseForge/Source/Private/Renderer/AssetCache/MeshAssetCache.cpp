#include "Renderer/AssetCache/MeshAssetCache.h"

#include "Assets/GltfMeshImporter.h"
#include "Core/Diagnostics.h"

#include <exception>

namespace PulseForge
{
	MeshAssetCache::MeshAssetCache(
		RenderDevice& Runtime,
		std::filesystem::path ProjectRoot,
		const AssetRegistry& Registry,
		DiagnosticRecorder* Diagnostics)
		: m_Runtime(Runtime), m_ProjectRoot(std::move(ProjectRoot)), m_Registry(Registry), m_Diagnostics(Diagnostics)
	{
	}

	MeshAssetCache::~MeshAssetCache()
	{
		Clear();
	}

	std::expected<std::reference_wrapper<const Mesh>, MeshAssetError> MeshAssetCache::GetOrLoad(const AssetID& Asset)
	{
		if (Asset.IsNil())
		{
			return std::unexpected(MeshAssetError{ MeshAssetErrorCode::AssetNotFound, Asset, "Cannot load a mesh with a nil asset UUID" });
		}
		if (!m_Registry.Find(Asset))
		{
			return std::unexpected(MeshAssetError{ MeshAssetErrorCode::AssetNotFound, Asset,
				"Mesh asset UUID " + Asset.ToString() + " is not present in the project asset registry" });
		}

		if (const auto Existing = m_Meshes.find(Asset); Existing != m_Meshes.end())
		{
			++m_CacheHits;
			return std::cref(*Existing->second);
		}
		++m_CacheMisses;

		auto ImportedMesh = [&]()
		{
			ScopedDiagnosticTimer Timer(m_Diagnostics, "Mesh asset import", "MeshAssetCache");
			return GltfMeshImporter::ImportStaticPrimitive(Asset, m_ProjectRoot, m_Registry);
		}();
		if (!ImportedMesh)
		{
			return std::unexpected(MeshAssetError{ MeshAssetErrorCode::ImportFailed, Asset, ImportedMesh.error().Message });
		}

		const MeshDesc Description = ImportedMesh->GetMeshDescription();
		auto CreatedMesh = [&]()
		{
			ScopedDiagnosticTimer Timer(m_Diagnostics, "Mesh GPU resource creation", "MeshAssetCache");
			return m_Runtime.CreateMesh(Description);
		}();
		if (!CreatedMesh)
		{
			return std::unexpected(MeshAssetError{ MeshAssetErrorCode::MeshCreationFailed, Asset, CreatedMesh.error().Message });
		}

		try
		{
			auto [Inserted, WasInserted] = m_Meshes.try_emplace(Asset, std::move(CreatedMesh.value()));
			if (!WasInserted)
			{
				return std::unexpected(MeshAssetError{
					MeshAssetErrorCode::CacheFailure, Asset, "Mesh asset UUID was already present during cache insertion" });
			}
			if (m_Diagnostics)
				(void)m_Diagnostics->RecordEvent(DiagnosticEventKind::Resource, "Mesh cache resource created", "MeshAssetCache", "objects", 1);
			return std::cref(*Inserted->second);
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(MeshAssetError{
				MeshAssetErrorCode::CacheFailure, Asset, std::string("Could not retain the uploaded mesh resource: ") + Exception.what() });
		}
	}

	void MeshAssetCache::Clear() noexcept
	{
		const uint64_t RetiredCount = m_Meshes.size();
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
} // namespace PulseForge
