#pragma once

#include "Core/Core.h"

#include <expected>
#include <memory>
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
		InvalidPlayback
	};

	struct AudioError
	{
		AudioErrorCode Code;
		std::string Message;
	};

	enum class AudioOutputBackend : uint8_t
	{
		Default,
		Null
	};

	struct AudioEngineDesc
	{
		AudioOutputBackend OutputBackend = AudioOutputBackend::Default;
	};

	struct AudioPlaybackDesc
	{
		float Volume = 1.0f;
		bool Looping = false;
		bool Spatialized = false;
		glm::vec3 Position{ 0.0f };

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
		[[nodiscard]] std::expected<AudioClip, AudioError> CreateClip(
			std::string_view DebugName,
			std::span<const std::byte> EncodedData) const;
		[[nodiscard]] std::expected<AudioPlayback, AudioError> Play(
			const AudioClip& Clip,
			const AudioPlaybackDesc& Description = {}) const;
		[[nodiscard]] std::expected<void, AudioError> SetListenerPosition(const glm::vec3& Position);
		[[nodiscard]] std::expected<void, AudioError> SetListenerDirection(const glm::vec3& Direction);

	private:
		struct Impl;
		explicit AudioEngine(std::shared_ptr<Impl> Implementation);
		std::shared_ptr<Impl> m_Impl;

		friend class AudioPlayback;
	};
}
