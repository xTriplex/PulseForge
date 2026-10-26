#include "Assets/MaterialAssetCache.h"

#if !defined(PF_COOKED_RUNTIME_ONLY)
#include "Assets/MaterialAssetService.h"
#endif
#include "Core/ScopedProfileTimer.h"
#include "Runtime/CookedContentProvider.h"

#include <exception>

namespace PulseForge
{
#if !defined(PF_COOKED_RUNTIME_ONLY)
	MaterialAssetCache::MaterialAssetCache(std::filesystem::path ProjectRoot, const AssetRegistry& Registry)
		: m_ProjectRoot(std::move(ProjectRoot)), m_Registry(&Registry)
	{
	}
#endif

	MaterialAssetCache::MaterialAssetCache(std::shared_ptr<const CookedContentProvider> Provider)
		: m_CookedProvider(std::move(Provider))
	{
	}

	bool MaterialAssetCache::IsRegistered(const AssetID& Asset) const noexcept
	{
		if (Asset.IsNil()) return false;
		if (m_CookedProvider) return m_CookedProvider->FindMaterial(Asset) != nullptr;
#if !defined(PF_COOKED_RUNTIME_ONLY)
		return m_Registry && m_Registry->Find(Asset).has_value();
#else
		return false;
#endif
	}

	std::expected<std::reference_wrapper<const MaterialAssetDesc>, MaterialAssetError> MaterialAssetCache::GetOrLoad(
		const AssetID& Asset)
	{
		if (Asset.IsNil())
			return std::unexpected(MaterialAssetError{ MaterialAssetErrorCode::AssetNotFound, Asset, "Cannot load a material with a nil asset UUID" });
		if (const auto Existing = m_Materials.find(Asset); Existing != m_Materials.end())
			return std::cref(Existing->second);

		Detail::ScopedProfileTimer Timer("MaterialAssetCache first load");
		std::expected<MaterialAssetDesc, MaterialAssetError> Material = std::unexpected(MaterialAssetError{
			MaterialAssetErrorCode::AssetNotFound, Asset, "No source registry or cooked provider is available" });
		if (m_CookedProvider)
		{
			auto Product = m_CookedProvider->LoadMaterialProduct(Asset);
			if (!Product)
			{
				const MaterialAssetErrorCode Code = Product.error().Code == CookedAssetCatalogErrorCode::AssetNotFound
					? MaterialAssetErrorCode::AssetNotFound : MaterialAssetErrorCode::InvalidDocument;
				return std::unexpected(MaterialAssetError{ Code, Asset, Product.error().Message });
			}
			MaterialAssetDesc Description;
			Description.BaseColorTexture = Product->BaseColorTexture;
			Description.MetallicRoughnessTexture = Product->MetallicRoughnessTexture;
			Description.NormalTexture = Product->NormalTexture;
			Description.OcclusionTexture = Product->OcclusionTexture;
			Description.EmissiveTexture = Product->EmissiveTexture;
			for (int Component = 0; Component < 4; ++Component)
				Description.BaseColorFactor[Component] = Product->BaseColorFactor[Component];
			Description.MetallicFactor = Product->MetallicFactor;
			Description.RoughnessFactor = Product->RoughnessFactor;
			Description.NormalScale = Product->NormalScale;
			Description.OcclusionStrength = Product->OcclusionStrength;
			for (int Component = 0; Component < 3; ++Component)
				Description.EmissiveFactor[Component] = Product->EmissiveFactor[Component];
			switch (Product->AlphaMode)
			{
				case CookedMaterialAlphaMode::Opaque: Description.AlphaMode = MaterialAlphaMode::Opaque; break;
				case CookedMaterialAlphaMode::Mask: Description.AlphaMode = MaterialAlphaMode::Mask; break;
				case CookedMaterialAlphaMode::Blend: Description.AlphaMode = MaterialAlphaMode::Blend; break;
			}
			Description.AlphaCutoff = Product->AlphaCutoff;
			Description.DoubleSided = Product->DoubleSided;
			Material = std::move(Description);
		}
#if !defined(PF_COOKED_RUNTIME_ONLY)
		else if (m_Registry)
			Material = MaterialAssetService::Load(Asset, m_ProjectRoot, *m_Registry);
#endif
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
