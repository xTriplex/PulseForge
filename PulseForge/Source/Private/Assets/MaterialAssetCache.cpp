#include "Core/PulseForgePCH.h"
#include "Assets/MaterialAssetCache.h"

#include "Assets/MaterialAssetService.h"

#include <exception>
#include <utility>

namespace PulseForge
{
	MaterialAssetCache::MaterialAssetCache(std::filesystem::path ProjectRoot, const AssetRegistry& Registry)
		: m_ProjectRoot(std::move(ProjectRoot)), m_Registry(Registry)
	{
	}

	std::expected<std::reference_wrapper<const MaterialAssetDesc>, MaterialAssetError> MaterialAssetCache::GetOrLoad(
		const AssetID& Asset)
	{
		if (Asset.IsNil())
			return std::unexpected(MaterialAssetError{ MaterialAssetErrorCode::AssetNotFound, Asset, "Cannot load a material with a nil asset UUID" });
		if (const auto Existing = m_Materials.find(Asset); Existing != m_Materials.end())
			return std::cref(Existing->second);

		auto Material = MaterialAssetService::Load(Asset, m_ProjectRoot, m_Registry);
		if (!Material)
			return std::unexpected(std::move(Material.error()));

		try
		{
			auto [Inserted, WasInserted] = m_Materials.try_emplace(Asset, std::move(*Material));
			if (!WasInserted)
				return std::unexpected(MaterialAssetError{ MaterialAssetErrorCode::CacheFailure, Asset, "Material was already present during cache insertion" });
			return std::cref(Inserted->second);
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(MaterialAssetError{
				MaterialAssetErrorCode::CacheFailure,
				Asset,
				std::string("Could not retain the loaded material description: ") + Exception.what() });
		}
	}

	void MaterialAssetCache::Clear() noexcept
	{
		m_Materials.clear();
	}
}
