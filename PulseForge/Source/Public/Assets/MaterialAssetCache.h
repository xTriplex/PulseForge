#pragma once

#include "Assets/AssetRegistry.h"
#include "Assets/MaterialAsset.h"
#include "Core/Core.h"

#include <cstddef>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <unordered_map>

namespace PulseForge
{
	class CookedContentProvider;
	class DiagnosticRecorder;

	// Per-project CPU description cache. Source mode borrows its registry; cooked mode retains its provider.
	// References returned by GetOrLoad remain valid until Clear().
	class PULSEFORGE_API MaterialAssetCache final
	{
	public:
		MaterialAssetCache(std::filesystem::path ProjectRoot, const AssetRegistry& Registry,
			DiagnosticRecorder* Diagnostics = nullptr);
		explicit MaterialAssetCache(
			std::shared_ptr<const CookedContentProvider> Provider, DiagnosticRecorder* Diagnostics = nullptr);
		MaterialAssetCache(const MaterialAssetCache&) = delete;
		MaterialAssetCache& operator=(const MaterialAssetCache&) = delete;

		[[nodiscard]] std::expected<std::reference_wrapper<const MaterialAssetDesc>, MaterialAssetError> GetOrLoad(
			const AssetID& Asset);
		[[nodiscard]] bool IsRegistered(const AssetID& Asset) const noexcept;
		void Clear() noexcept;
		[[nodiscard]] size_t GetLoadedCount() const noexcept { return m_Materials.size(); }

	private:
		std::filesystem::path m_ProjectRoot;
		const AssetRegistry* m_Registry = nullptr;
		std::shared_ptr<const CookedContentProvider> m_CookedProvider;
		DiagnosticRecorder* m_Diagnostics = nullptr;
		std::unordered_map<AssetID, MaterialAssetDesc, UUIDHash> m_Materials;
	};
}
