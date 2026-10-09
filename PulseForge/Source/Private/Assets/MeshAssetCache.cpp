#include "Core/PulseForgePCH.h"
#include "Assets/MeshAssetCache.h"

#include "Assets/GltfMeshImporter.h"
#include "Core/Application.h"
#include "Core/ScopedProfileTimer.h"

#include <exception>
#include <utility>

namespace PulseForge
{
	MeshAssetCache::MeshAssetCache(
		Application& Runtime,
		std::filesystem::path ProjectRoot,
		const AssetRegistry& Registry)
		: m_Runtime(Runtime), m_ProjectRoot(std::move(ProjectRoot)), m_Registry(Registry)
	{
	}

	MeshAssetCache::~MeshAssetCache() = default;

	std::expected<std::reference_wrapper<const Mesh>, MeshAssetError> MeshAssetCache::GetOrLoad(const AssetID& Asset)
	{
		if (Asset.IsNil())
		{
			return std::unexpected(MeshAssetError{
				MeshAssetErrorCode::AssetNotFound,
				Asset,
				"Cannot load a mesh with a nil asset UUID" });
		}
		if (!m_Registry.Find(Asset))
		{
			return std::unexpected(MeshAssetError{
				MeshAssetErrorCode::AssetNotFound,
				Asset,
				"Mesh asset UUID " + Asset.ToString() + " is not present in the project asset registry" });
		}

		if (const auto Existing = m_Meshes.find(Asset); Existing != m_Meshes.end())
			return std::cref(*Existing->second);

		Detail::ScopedProfileTimer Timer("MeshAssetCache first load (import + GPU mesh upload)");
		auto ImportedMesh = GltfMeshImporter::ImportStaticPrimitive(Asset, m_ProjectRoot, m_Registry);
		if (!ImportedMesh)
		{
			return std::unexpected(MeshAssetError{
				MeshAssetErrorCode::ImportFailed,
				Asset,
				ImportedMesh.error().Message });
		}

		const MeshDesc Description = ImportedMesh->GetMeshDescription();
		auto CreatedMesh = m_Runtime.CreateMesh(Description);
		if (!CreatedMesh)
		{
			return std::unexpected(MeshAssetError{
				MeshAssetErrorCode::MeshCreationFailed,
				Asset,
				CreatedMesh.error().Message });
		}

		try
		{
			auto [Inserted, WasInserted] = m_Meshes.try_emplace(Asset, std::move(CreatedMesh.value()));
			if (!WasInserted)
			{
				return std::unexpected(MeshAssetError{
					MeshAssetErrorCode::CacheFailure,
					Asset,
					"Mesh asset UUID was already present during cache insertion" });
			}
			return std::cref(*Inserted->second);
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(MeshAssetError{
				MeshAssetErrorCode::CacheFailure,
				Asset,
				std::string("Could not retain the uploaded mesh resource: ") + Exception.what() });
		}
	}

	void MeshAssetCache::Clear() noexcept
	{
		m_Meshes.clear();
	}
}
