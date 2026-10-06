#pragma once

#include "Assets/AssetRegistry.h"
#include "Audio/AudioEngine.h"
#include "Core/Core.h"

#include <expected>
#include <filesystem>
#include <string>
#include <unordered_map>

namespace PulseForge
{
	enum class AudioAssetErrorCode : uint8_t
	{
		AssetNotFound,
		PathResolutionFailed,
		FileReadFailed,
		ClipCreationFailed,
		CacheFailure
	};

	struct AudioAssetError
	{
		AudioAssetErrorCode Code;
		AssetID Asset;
		std::string Message;
	};

	// Per-project audio clip cache. The registry and audio engine must outlive this cache.
	// Audio clips are looked up by stable UUID; source paths remain private to this implementation.
	class PULSEFORGE_API AudioAssetCache final
	{
	public:
		AudioAssetCache(AudioEngine& Engine, std::filesystem::path ProjectRoot, const AssetRegistry& Registry);
		AudioAssetCache(const AudioAssetCache&) = delete;
		AudioAssetCache& operator=(const AudioAssetCache&) = delete;

		[[nodiscard]] std::expected<AudioClip, AudioAssetError> GetOrLoad(const AssetID& Asset);
		void Clear() noexcept;
		[[nodiscard]] size_t GetLoadedCount() const noexcept { return m_Clips.size(); }

	private:
		AudioEngine& m_Engine;
		std::filesystem::path m_ProjectRoot;
		const AssetRegistry& m_Registry;
		std::unordered_map<AssetID, AudioClip, UUIDHash> m_Clips;
	};
}
