#pragma once

#include "Assets/AssetRegistry.h"
#include "Renderer/Mesh.h"
#include "Renderer/RenderDevice.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>

namespace PulseForge
{
	class DiagnosticRecorder;

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

	// Per-project runtime GPU cache. The rendering device, project registry and optional diagnostics recorder must outlive this cache.
	// Returned mesh references remain valid until Clear() or cache destruction. The cache is not thread-safe.
	class MeshAssetCache final
	{
	public:
		MeshAssetCache(
			RenderDevice& Runtime,
			std::filesystem::path ProjectRoot,
			const AssetRegistry& Registry,
			DiagnosticRecorder* Diagnostics = nullptr);
		~MeshAssetCache();
		MeshAssetCache(const MeshAssetCache&) = delete;
		MeshAssetCache& operator=(const MeshAssetCache&) = delete;
		MeshAssetCache(MeshAssetCache&&) = delete;
		MeshAssetCache& operator=(MeshAssetCache&&) = delete;

		[[nodiscard]] std::expected<std::reference_wrapper<const Mesh>, MeshAssetError> GetOrLoad(const AssetID& Asset);
		void Clear() noexcept;
		[[nodiscard]] size_t GetLoadedCount() const noexcept { return m_Meshes.size(); }

	private:
		RenderDevice& m_Runtime;
		std::filesystem::path m_ProjectRoot;
		const AssetRegistry& m_Registry;
		DiagnosticRecorder* m_Diagnostics = nullptr;
		uint64_t m_CacheHits = 0;
		uint64_t m_CacheMisses = 0;
		std::unordered_map<AssetID, MeshHandle, UUIDHash> m_Meshes;
	};
} // namespace PulseForge
