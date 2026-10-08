#pragma once

#include "Assets/AssetRegistry.h"
#include "Core/Core.h"
#include "Renderer/EnvironmentLighting.h"
#include "Renderer/Texture.h"

#include <expected>
#include <filesystem>
#include <functional>
#include <unordered_map>

namespace PulseForge
{
	class Application;

	struct EnvironmentLightingTextures
	{
		TextureHandle Environment;
		TextureHandle DiffuseIrradiance;
		TextureHandle PrefilteredSpecular;
		TextureHandle BrdfIntegrationLut;
	};

	struct EnvironmentLightingCacheError
	{
		AssetID Asset;
		std::string Message;
	};

	// Owns source-derived environment textures for the lifetime of its renderer/project cache.
	class PULSEFORGE_API EnvironmentLightingCache final
	{
	public:
		EnvironmentLightingCache(Application& Runtime, std::filesystem::path ProjectRoot, const AssetRegistry& Registry);
		[[nodiscard]] std::expected<std::reference_wrapper<const EnvironmentLightingTextures>, EnvironmentLightingCacheError>
			GetOrLoad(const AssetID& Asset);
		[[nodiscard]] size_t GetLoadedCount() const noexcept { return m_Textures.size(); }

	private:
		Application& m_Runtime;
		std::filesystem::path m_ProjectRoot;
		const AssetRegistry& m_Registry;
		std::unordered_map<AssetID, EnvironmentLightingTextures, UUIDHash> m_Textures;
	};
}
