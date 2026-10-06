#pragma once

#include "Assets/AssetRegistry.h"
#include "Core/Core.h"
#include "Renderer/Mesh.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>

namespace PulseForge
{
	class Application;

	enum class MeshAssetErrorCode : uint8_t
	{
		AssetNotFound,
		ImportFailed,
		MeshCreationFailed,
		CacheFailure
	};

	struct MeshAssetError
	{
		MeshAssetErrorCode Code;
		AssetID Asset;
		std::string Message;
	};

	// Per-project runtime GPU cache. Application, project registry, and renderer must outlive this cache.
	// Returned mesh references remain valid until Clear() or cache destruction. The cache is not thread-safe.
	class PULSEFORGE_API MeshAssetCache final
	{
	public:
		MeshAssetCache(Application& Runtime, std::filesystem::path ProjectRoot, const AssetRegistry& Registry);
		~MeshAssetCache();
		MeshAssetCache(const MeshAssetCache&) = delete;
		MeshAssetCache& operator=(const MeshAssetCache&) = delete;
		MeshAssetCache(MeshAssetCache&&) = delete;
		MeshAssetCache& operator=(MeshAssetCache&&) = delete;

		[[nodiscard]] std::expected<std::reference_wrapper<const Mesh>, MeshAssetError> GetOrLoad(const AssetID& Asset);
		void Clear() noexcept;
		[[nodiscard]] size_t GetLoadedCount() const noexcept { return m_Meshes.size(); }

	private:
		Application& m_Runtime;
		std::filesystem::path m_ProjectRoot;
		const AssetRegistry& m_Registry;
		std::unordered_map<AssetID, MeshHandle, UUIDHash> m_Meshes;
	};
}
