#pragma once

#include "Core/Core.h"

#include <cstddef>
#include <expected>
#include <memory>
#include <string>

namespace PulseForge
{
	class AudioAssetCache;
	class AudioEngine;
	class Scene;
	class UUID;

	enum class AudioSceneRuntimeErrorCode : uint8_t
	{
		AlreadyRunning,
		NotRunning,
		DifferentScene,
		InvalidAudioEngine,
		InvalidComponent,
		MultiplePrimaryListeners,
		SceneOperationFailed,
		AssetLoadFailed,
		PlaybackFailed,
		MissingAudioSource,
		MissingPlayback
	};

	struct AudioSceneRuntimeError
	{
		AudioSceneRuntimeErrorCode Code;
		std::string Message;
	};

	class PULSEFORGE_API AudioSceneRuntime final
	{
	public:
		// The engine, asset cache, and started scene must outlive this runtime; keep the scene at the same address while started.
		AudioSceneRuntime(AudioEngine& Engine, AudioAssetCache& Assets);
		~AudioSceneRuntime();
		AudioSceneRuntime(const AudioSceneRuntime&) = delete;
		AudioSceneRuntime& operator=(const AudioSceneRuntime&) = delete;

		[[nodiscard]] std::expected<void, AudioSceneRuntimeError> Start(Scene& Source);
		void Stop() noexcept;
		[[nodiscard]] bool IsRunning() const noexcept;
		[[nodiscard]] size_t GetPlaybackCount() const noexcept;
		[[nodiscard]] std::expected<void, AudioSceneRuntimeError> Advance(Scene& Source);
		[[nodiscard]] std::expected<void, AudioSceneRuntimeError> Play(UUID Entity);
		[[nodiscard]] std::expected<void, AudioSceneRuntimeError> Pause(UUID Entity);
		[[nodiscard]] std::expected<void, AudioSceneRuntimeError> Resume(UUID Entity);
		[[nodiscard]] std::expected<void, AudioSceneRuntimeError> StopPlayback(UUID Entity);

	private:
		struct Impl;
		AudioEngine& m_Engine;
		AudioAssetCache& m_Assets;
		std::unique_ptr<Impl> m_Impl;
	};
}
