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
	class Entity;
	class Scene;
	class UUID;

	enum class AudioControlOperation : uint8_t
	{
		Play,
		Pause,
		Resume,
		Stop
	};

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
		MissingPlayback,
		CommandQueueFull,
		InvalidCommand
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
		AudioSceneRuntime(
			AudioEngine& Engine,
			AudioAssetCache& Assets,
			uint64_t SessionGeneration = 1,
			uint64_t SceneGeneration = 1);
		~AudioSceneRuntime();
		AudioSceneRuntime(const AudioSceneRuntime&) = delete;
		AudioSceneRuntime& operator=(const AudioSceneRuntime&) = delete;

		[[nodiscard]] std::expected<void, AudioSceneRuntimeError> Start(Scene& Source, bool DeferActivation = false);
		[[nodiscard]] std::expected<void, AudioSceneRuntimeError> Activate();
		// Suspend only voices that were started; resume restores that exact set unless global/entity pause now applies.
		[[nodiscard]] std::expected<void, AudioSceneRuntimeError> SuspendForTransition();
		[[nodiscard]] std::expected<void, AudioSceneRuntimeError> ResumeAfterTransition();
		[[nodiscard]] std::expected<void, AudioSceneRuntimeError> QueueControl(
			const Entity& Target,
			AudioControlOperation Operation);
		[[nodiscard]] std::expected<void, AudioSceneRuntimeError> ProcessPendingCommands();
		void Stop() noexcept;
		[[nodiscard]] bool IsRunning() const noexcept;
		[[nodiscard]] size_t GetPlaybackCount() const noexcept;
		[[nodiscard]] std::expected<bool, AudioSceneRuntimeError> IsPlaybackPlaying(UUID Entity) const;
		[[nodiscard]] std::expected<void, AudioSceneRuntimeError> SetPaused(bool Paused);
		[[nodiscard]] std::expected<void, AudioSceneRuntimeError> Advance(Scene& Source);
		[[nodiscard]] std::expected<void, AudioSceneRuntimeError> Play(UUID Entity);
		[[nodiscard]] std::expected<void, AudioSceneRuntimeError> Pause(UUID Entity);
		[[nodiscard]] std::expected<void, AudioSceneRuntimeError> Resume(UUID Entity);
		[[nodiscard]] std::expected<void, AudioSceneRuntimeError> StopPlayback(UUID Entity);

	private:
		struct Impl;
		AudioEngine& m_Engine;
		AudioAssetCache& m_Assets;
		uint64_t m_SessionGeneration = 0;
		uint64_t m_SceneGeneration = 0;
		std::unique_ptr<Impl> m_Impl;
	};
}
