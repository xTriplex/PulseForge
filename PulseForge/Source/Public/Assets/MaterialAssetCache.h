#pragma once

#include "Assets/AssetRegistry.h"
#include "Assets/MaterialAsset.h"
#include "Core/Core.h"

#include <cstddef>
#include <expected>
#include <filesystem>
#include <functional>
#include <unordered_map>

namespace PulseForge
{
	// Per-project CPU description cache. The registry must outlive the cache; references remain valid until Clear().
	class PULSEFORGE_API MaterialAssetCache final
	{
	public:
		MaterialAssetCache(std::filesystem::path ProjectRoot, const AssetRegistry& Registry);
		MaterialAssetCache(const MaterialAssetCache&) = delete;
		MaterialAssetCache& operator=(const MaterialAssetCache&) = delete;

		[[nodiscard]] std::expected<std::reference_wrapper<const MaterialAssetDesc>, MaterialAssetError> GetOrLoad(
			const AssetID& Asset);
		void Clear() noexcept;
		[[nodiscard]] size_t GetLoadedCount() const noexcept { return m_Materials.size(); }

	private:
		std::filesystem::path m_ProjectRoot;
		const AssetRegistry& m_Registry;
		std::unordered_map<AssetID, MaterialAssetDesc, UUIDHash> m_Materials;
	};
}
