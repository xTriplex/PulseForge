#pragma once

#include "Audio/AudioBus.h"
#include "Core/Core.h"

#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <glm/glm.hpp>

namespace PulseForge
{
	enum class AudioErrorCode : uint8_t
	{
		InvalidDescription,
		InvalidClip,
		DeviceInitializationFailed,
		PlaybackInitializationFailed,
		PlaybackStartFailed,
		PlaybackControlFailed,
		InvalidPlayback,
		WrongThread,
		InvalidBus,
		OfflineRenderingUnavailable
	};

	struct AudioError
	{
		AudioErrorCode Code;
		std::string Message;
	};

	enum class AudioOutputBackend : uint8_t
	{
		Default,
		Null,
		Offline
	};

	enum class AudioDeviceEvent : uint8_t
	{
		Started,
		Stopped,
		Rerouted,
		InterruptionBegan,
		InterruptionEnded,
		Unlocked
	};

	struct AudioEngineDesc
	{
		AudioOutputBackend OutputBackend = AudioOutputBackend::Default;
	};

	struct AudioEngineMemoryStats final
	{
		uint64_t RetainedClipCount = 0;
		uint64_t RetainedEncodedClipBytes = 0;
		uint64_t LivePlaybackCount = 0;
		uint64_t KnownPlaybackObjectBytes = 0;
		uint64_t KnownDecoderObjectBytes = 0;
		// miniaudio's codec/backend allocations are allocator-owned and are not exposed as an exact byte count.
		std::optional<uint64_t> BackendAllocatedBytes;
	};

	struct AudioPlaybackDesc
	{
		float Volume = 1.0f;
		bool Looping = false;
		bool Spatialized = false;
		glm::vec3 Position{ 0.0f };
		AudioBus Bus = AudioBus::Effects;
		// Prepares a voice without making it audible until AudioPlayback::Resume is called.
		bool StartPaused = false;

		[[nodiscard]] PULSEFORGE_API std::expected<void, AudioError> Validate() const;
	};

	class AudioEngine;
	class AudioPlayback;

	class PULSEFORGE_API AudioClip final
	{
	public:
		AudioClip() = default;
		[[nodiscard]] bool IsValid() const noexcept { return m_Impl != nullptr; }
		[[nodiscard]] std::string_view GetDebugName() const noexcept;

	private:
		struct Impl;
		explicit AudioClip(std::shared_ptr<const Impl> Implementation);
		std::shared_ptr<const Impl> m_Impl;

		friend class AudioEngine;
		friend class AudioPlayback;
	};

	// A playback owns its sound and keeps the audio device and clip alive until it is released.
	class PULSEFORGE_API AudioPlayback final
	{
	public:
		AudioPlayback();
		~AudioPlayback();
		AudioPlayback(AudioPlayback&&) noexcept;
		AudioPlayback& operator=(AudioPlayback&&) noexcept;
		AudioPlayback(const AudioPlayback&) = delete;
		AudioPlayback& operator=(const AudioPlayback&) = delete;

		[[nodiscard]] bool IsValid() const noexcept;
		[[nodiscard]] bool IsPlaying() const noexcept;
		[[nodiscard]] bool IsAtEnd() const noexcept;
		[[nodiscard]] std::expected<void, AudioError> Pause();
		[[nodiscard]] std::expected<void, AudioError> Resume();
		[[nodiscard]] std::expected<void, AudioError> Stop();
		[[nodiscard]] std::expected<void, AudioError> SetVolume(float Volume);
		[[nodiscard]] std::expected<void, AudioError> SetLooping(bool Looping);
		[[nodiscard]] std::expected<void, AudioError> SetSpatialized(bool Spatialized);
		[[nodiscard]] std::expected<void, AudioError> SetPosition(const glm::vec3& Position);

	private:
		struct Impl;
		explicit AudioPlayback(std::unique_ptr<Impl> Implementation);
		std::unique_ptr<Impl> m_Impl;

		friend class AudioEngine;
	};

	class PULSEFORGE_API AudioEngine final
	{
	public:
		[[nodiscard]] static std::expected<AudioEngine, AudioError> Create(AudioEngineDesc Description = {});
		~AudioEngine();
		AudioEngine(AudioEngine&&) noexcept;
		AudioEngine& operator=(AudioEngine&&) noexcept;
		AudioEngine(const AudioEngine&) = delete;
		AudioEngine& operator=(const AudioEngine&) = delete;

		[[nodiscard]] bool IsInitialized() const noexcept;
		// Changes when a replacement engine/device instance is created; scene owners use it to retire old voices.
		[[nodiscard]] uint64_t GetDeviceGeneration() const noexcept;
		// Reports exact retained encoded payload and known C++ object sizes; backend-owned allocations remain unknown.
		[[nodiscard]] AudioEngineMemoryStats GetMemoryStats() const noexcept;
		[[nodiscard]] std::expected<AudioClip, AudioError> CreateClip(
			std::string_view DebugName,
			std::span<const std::byte> EncodedData) const;
		[[nodiscard]] std::expected<AudioPlayback, AudioError> Play(
			const AudioClip& Clip,
			const AudioPlaybackDesc& Description = {}) const;
		// Mix, playback control, and listener changes are owner-thread operations.
		[[nodiscard]] std::expected<void, AudioError> SetMasterGain(float Gain);
		[[nodiscard]] std::expected<void, AudioError> SetBusGain(AudioBus Bus, float Gain);
		[[nodiscard]] std::expected<void, AudioError> SetMasterMuted(bool Muted);
		[[nodiscard]] std::expected<void, AudioError> SetBusMuted(AudioBus Bus, bool Muted);
		[[nodiscard]] std::expected<AudioBusMix, AudioError> GetBusMix(AudioBus Bus) const;
		// Offline output is fixed at 48 kHz stereo float. Span length is interleaved and must contain whole frames.
		[[nodiscard]] std::expected<size_t, AudioError> RenderOfflineFrames(std::span<float> InterleavedSamples);
		// The miniaudio callback only records the latest event; the session owner consumes it and performs recovery.
		[[nodiscard]] std::optional<AudioDeviceEvent> ConsumeDeviceEvent() noexcept;
		[[nodiscard]] std::expected<void, AudioError> SetListenerPosition(const glm::vec3& Position);
		[[nodiscard]] std::expected<void, AudioError> SetListenerDirection(const glm::vec3& Direction);

	private:
		struct Impl;
		explicit AudioEngine(std::shared_ptr<Impl> Implementation);
		std::shared_ptr<Impl> m_Impl;

		friend class AudioClip;
		friend class AudioPlayback;
	};
}
