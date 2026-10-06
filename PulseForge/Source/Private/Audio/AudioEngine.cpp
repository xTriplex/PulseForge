#include "Core/PulseForgePCH.h"
#include "Audio/AudioEngine.h"

#include <miniaudio.h>

#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace PulseForge
{
	namespace
	{
		constexpr size_t MaxAudioClipBytes = 256u * 1024u * 1024u;

		AudioError MakeError(AudioErrorCode Code, std::string Message)
		{
			return { Code, std::move(Message) };
		}

		std::string MiniaudioError(std::string_view Operation, ma_result Result)
		{
			return std::string(Operation) + " failed: " + ma_result_description(Result) +
				" (miniaudio result " + std::to_string(static_cast<int>(Result)) + ")";
		}

		bool IsFinite(const glm::vec3& Value)
		{
			return std::isfinite(Value.x) && std::isfinite(Value.y) && std::isfinite(Value.z);
		}
	}

	struct AudioClip::Impl final
	{
		std::string DebugName;
		std::vector<std::byte> EncodedData;
	};

	struct AudioEngine::Impl final
	{
		ma_context Context{};
		bool ContextInitialized = false;
		ma_engine Engine{};
		bool EngineInitialized = false;

		~Impl()
		{
			if (EngineInitialized)
				ma_engine_uninit(&Engine);
			if (ContextInitialized)
				ma_context_uninit(&Context);
		}
	};

	struct AudioPlayback::Impl final
	{
		std::shared_ptr<AudioEngine::Impl> Device;
		std::shared_ptr<const AudioClip::Impl> Clip;
		ma_decoder Decoder{};
		bool DecoderInitialized = false;
		ma_sound Sound{};
		bool SoundInitialized = false;

		~Impl()
		{
			if (SoundInitialized)
			{
				(void)ma_sound_stop(&Sound);
				ma_sound_uninit(&Sound);
			}
			if (DecoderInitialized)
				(void)ma_decoder_uninit(&Decoder);
		}
	};

	std::expected<void, AudioError> AudioPlaybackDesc::Validate() const
	{
		if (!std::isfinite(Volume) || Volume < 0.0f || Volume > 1.0f)
			return std::unexpected(MakeError(AudioErrorCode::InvalidDescription, "Audio volume must be finite and between zero and one"));
		if (!IsFinite(Position))
			return std::unexpected(MakeError(AudioErrorCode::InvalidDescription, "Audio position must contain finite values"));
		return {};
	}

	AudioClip::AudioClip(std::shared_ptr<const Impl> Implementation)
		: m_Impl(std::move(Implementation))
	{
	}

	std::string_view AudioClip::GetDebugName() const noexcept
	{
		return m_Impl ? std::string_view(m_Impl->DebugName) : std::string_view{};
	}

	AudioPlayback::AudioPlayback() = default;
	AudioPlayback::~AudioPlayback() = default;
	AudioPlayback::AudioPlayback(AudioPlayback&&) noexcept = default;
	AudioPlayback& AudioPlayback::operator=(AudioPlayback&&) noexcept = default;

	AudioPlayback::AudioPlayback(std::unique_ptr<Impl> Implementation)
		: m_Impl(std::move(Implementation))
	{
	}

	bool AudioPlayback::IsValid() const noexcept
	{
		return m_Impl && m_Impl->SoundInitialized;
	}

	bool AudioPlayback::IsPlaying() const noexcept
	{
		return IsValid() && ma_sound_is_playing(&m_Impl->Sound) == MA_TRUE;
	}

	bool AudioPlayback::IsAtEnd() const noexcept
	{
		return IsValid() && ma_sound_at_end(&m_Impl->Sound) == MA_TRUE;
	}

	std::expected<void, AudioError> AudioPlayback::Pause()
	{
		if (!IsValid())
			return std::unexpected(MakeError(AudioErrorCode::InvalidPlayback, "Cannot pause an invalid audio playback"));
		const ma_result Result = ma_sound_stop(&m_Impl->Sound);
		if (Result != MA_SUCCESS)
			return std::unexpected(MakeError(AudioErrorCode::PlaybackControlFailed, MiniaudioError("Pausing audio playback", Result)));
		return {};
	}

	std::expected<void, AudioError> AudioPlayback::Resume()
	{
		if (!IsValid())
			return std::unexpected(MakeError(AudioErrorCode::InvalidPlayback, "Cannot resume an invalid audio playback"));
		const ma_result Result = ma_sound_start(&m_Impl->Sound);
		if (Result != MA_SUCCESS)
			return std::unexpected(MakeError(AudioErrorCode::PlaybackControlFailed, MiniaudioError("Resuming audio playback", Result)));
		return {};
	}

	std::expected<void, AudioError> AudioPlayback::Stop()
	{
		if (!IsValid())
			return std::unexpected(MakeError(AudioErrorCode::InvalidPlayback, "Cannot stop an invalid audio playback"));
		if (const ma_result Result = ma_sound_stop(&m_Impl->Sound); Result != MA_SUCCESS)
			return std::unexpected(MakeError(AudioErrorCode::PlaybackControlFailed, MiniaudioError("Stopping audio playback", Result)));
		if (const ma_result Result = ma_sound_seek_to_pcm_frame(&m_Impl->Sound, 0); Result != MA_SUCCESS)
			return std::unexpected(MakeError(AudioErrorCode::PlaybackControlFailed, MiniaudioError("Resetting audio playback", Result)));
		return {};
	}

	std::expected<void, AudioError> AudioPlayback::SetVolume(float Volume)
	{
		if (!IsValid())
			return std::unexpected(MakeError(AudioErrorCode::InvalidPlayback, "Cannot update an invalid audio playback"));
		if (!std::isfinite(Volume) || Volume < 0.0f || Volume > 1.0f)
			return std::unexpected(MakeError(AudioErrorCode::InvalidDescription, "Audio volume must be finite and between zero and one"));
		ma_sound_set_volume(&m_Impl->Sound, Volume);
		return {};
	}

	std::expected<void, AudioError> AudioPlayback::SetLooping(bool Looping)
	{
		if (!IsValid())
			return std::unexpected(MakeError(AudioErrorCode::InvalidPlayback, "Cannot update an invalid audio playback"));
		ma_sound_set_looping(&m_Impl->Sound, Looping ? MA_TRUE : MA_FALSE);
		return {};
	}

	std::expected<void, AudioError> AudioPlayback::SetSpatialized(bool Spatialized)
	{
		if (!IsValid())
			return std::unexpected(MakeError(AudioErrorCode::InvalidPlayback, "Cannot update an invalid audio playback"));
		ma_sound_set_spatialization_enabled(&m_Impl->Sound, Spatialized ? MA_TRUE : MA_FALSE);
		return {};
	}

	std::expected<void, AudioError> AudioPlayback::SetPosition(const glm::vec3& Position)
	{
		if (!IsValid())
			return std::unexpected(MakeError(AudioErrorCode::InvalidPlayback, "Cannot update an invalid audio playback"));
		if (!IsFinite(Position))
			return std::unexpected(MakeError(AudioErrorCode::InvalidDescription, "Audio position must contain finite values"));
		ma_sound_set_position(&m_Impl->Sound, Position.x, Position.y, Position.z);
		return {};
	}

	AudioEngine::AudioEngine(std::shared_ptr<Impl> Implementation)
		: m_Impl(std::move(Implementation))
	{
	}

	AudioEngine::~AudioEngine() = default;
	AudioEngine::AudioEngine(AudioEngine&&) noexcept = default;
	AudioEngine& AudioEngine::operator=(AudioEngine&&) noexcept = default;

	std::expected<AudioEngine, AudioError> AudioEngine::Create(AudioEngineDesc Description)
	{
		if (Description.OutputBackend != AudioOutputBackend::Default && Description.OutputBackend != AudioOutputBackend::Null)
			return std::unexpected(MakeError(AudioErrorCode::InvalidDescription, "Audio output backend is not supported"));

		try
		{
			auto Implementation = std::make_shared<Impl>();
			if (Description.OutputBackend == AudioOutputBackend::Null)
			{
				const ma_backend Backend = ma_backend_null;
				if (const ma_result Result = ma_context_init(&Backend, 1, nullptr, &Implementation->Context);
					Result != MA_SUCCESS)
				{
					return std::unexpected(MakeError(
						AudioErrorCode::DeviceInitializationFailed,
						MiniaudioError("Initializing the null audio context", Result)));
				}
				Implementation->ContextInitialized = true;
			}

			ma_engine_config EngineConfiguration = ma_engine_config_init();
			if (Implementation->ContextInitialized)
				EngineConfiguration.pContext = &Implementation->Context;
			if (const ma_result Result = ma_engine_init(&EngineConfiguration, &Implementation->Engine);
				Result != MA_SUCCESS)
			{
				return std::unexpected(MakeError(
					AudioErrorCode::DeviceInitializationFailed,
					MiniaudioError("Initializing the audio engine", Result)));
			}
			Implementation->EngineInitialized = true;
			return AudioEngine(std::move(Implementation));
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(MakeError(
				AudioErrorCode::DeviceInitializationFailed,
				std::string("Could not allocate audio engine state: ") + Exception.what()));
		}
	}

	bool AudioEngine::IsInitialized() const noexcept
	{
		return m_Impl && m_Impl->EngineInitialized;
	}

	std::expected<AudioClip, AudioError> AudioEngine::CreateClip(
		std::string_view DebugName,
		std::span<const std::byte> EncodedData) const
	{
		if (!m_Impl || !m_Impl->EngineInitialized)
			return std::unexpected(MakeError(
				AudioErrorCode::DeviceInitializationFailed,
				"Cannot create an audio clip without an initialized audio engine"));
		if (EncodedData.empty() || EncodedData.size() > MaxAudioClipBytes)
			return std::unexpected(MakeError(
				AudioErrorCode::InvalidClip,
				"Encoded audio data must be non-empty and no larger than 256 MiB"));

		ma_decoder Decoder{};
		if (const ma_result Result = ma_decoder_init_memory(EncodedData.data(), EncodedData.size(), nullptr, &Decoder);
			Result != MA_SUCCESS)
		{
			return std::unexpected(MakeError(
				AudioErrorCode::InvalidClip,
				MiniaudioError("Decoding audio clip", Result)));
		}

		ma_format Format = ma_format_unknown;
		ma_uint32 Channels = 0;
		ma_uint32 SampleRate = 0;
		const ma_result FormatResult = ma_decoder_get_data_format(&Decoder, &Format, &Channels, &SampleRate, nullptr, 0);
		(void)ma_decoder_uninit(&Decoder);
		if (FormatResult != MA_SUCCESS || Format == ma_format_unknown || Channels == 0 || SampleRate == 0)
			return std::unexpected(MakeError(
				AudioErrorCode::InvalidClip,
				FormatResult == MA_SUCCESS
					? "Decoded audio format did not provide a valid sample format, channel count, and sample rate"
					: MiniaudioError("Inspecting decoded audio format", FormatResult)));

		try
		{
			auto Clip = std::make_shared<AudioClip::Impl>();
			Clip->DebugName = DebugName.empty() ? "Audio clip" : std::string(DebugName);
			Clip->EncodedData.assign(EncodedData.begin(), EncodedData.end());
			return AudioClip(std::move(Clip));
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(MakeError(
				AudioErrorCode::InvalidClip,
				std::string("Could not retain encoded audio data: ") + Exception.what()));
		}
	}

	std::expected<AudioPlayback, AudioError> AudioEngine::Play(
		const AudioClip& Clip,
		const AudioPlaybackDesc& Description) const
	{
		if (!m_Impl || !m_Impl->EngineInitialized)
			return std::unexpected(MakeError(AudioErrorCode::DeviceInitializationFailed, "Cannot play audio without an initialized audio engine"));
		if (!Clip.m_Impl)
			return std::unexpected(MakeError(AudioErrorCode::InvalidClip, "Cannot play an empty audio clip handle"));
		if (auto Validation = Description.Validate(); !Validation)
			return std::unexpected(Validation.error());

		try
		{
			auto Playback = std::make_unique<AudioPlayback::Impl>();
			Playback->Device = m_Impl;
			Playback->Clip = Clip.m_Impl;
			if (const ma_result Result = ma_decoder_init_memory(
					Playback->Clip->EncodedData.data(),
					Playback->Clip->EncodedData.size(),
					nullptr,
					&Playback->Decoder);
				Result != MA_SUCCESS)
			{
				return std::unexpected(MakeError(
					AudioErrorCode::PlaybackInitializationFailed,
					MiniaudioError("Opening audio clip for playback", Result)));
			}
			Playback->DecoderInitialized = true;

			const ma_uint32 Flags = Description.Spatialized ? 0 : MA_SOUND_FLAG_NO_SPATIALIZATION;
			if (const ma_result Result = ma_sound_init_from_data_source(
					&m_Impl->Engine,
					&Playback->Decoder,
					Flags,
					nullptr,
					&Playback->Sound);
				Result != MA_SUCCESS)
			{
				return std::unexpected(MakeError(
					AudioErrorCode::PlaybackInitializationFailed,
					MiniaudioError("Creating audio playback", Result)));
			}
			Playback->SoundInitialized = true;
			ma_sound_set_volume(&Playback->Sound, Description.Volume);
			ma_sound_set_looping(&Playback->Sound, Description.Looping ? MA_TRUE : MA_FALSE);
			if (Description.Spatialized)
				ma_sound_set_position(&Playback->Sound, Description.Position.x, Description.Position.y, Description.Position.z);
			if (const ma_result Result = ma_sound_start(&Playback->Sound); Result != MA_SUCCESS)
			{
				return std::unexpected(MakeError(
					AudioErrorCode::PlaybackStartFailed,
					MiniaudioError("Starting audio playback", Result)));
			}
			return AudioPlayback(std::move(Playback));
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(MakeError(
				AudioErrorCode::PlaybackInitializationFailed,
				std::string("Could not allocate audio playback state: ") + Exception.what()));
		}
	}

	std::expected<void, AudioError> AudioEngine::SetListenerPosition(const glm::vec3& Position)
	{
		if (!IsInitialized())
			return std::unexpected(MakeError(
				AudioErrorCode::DeviceInitializationFailed,
				"Cannot update the listener without an initialized audio engine"));
		if (!IsFinite(Position))
			return std::unexpected(MakeError(AudioErrorCode::InvalidDescription, "Listener position must contain finite values"));
		ma_engine_listener_set_position(&m_Impl->Engine, 0, Position.x, Position.y, Position.z);
		return {};
	}

	std::expected<void, AudioError> AudioEngine::SetListenerDirection(const glm::vec3& Direction)
	{
		if (!IsInitialized())
			return std::unexpected(MakeError(
				AudioErrorCode::DeviceInitializationFailed,
				"Cannot update the listener without an initialized audio engine"));
		if (!IsFinite(Direction))
			return std::unexpected(MakeError(AudioErrorCode::InvalidDescription, "Listener direction must contain finite values"));
		const float Length = glm::length(Direction);
		if (!std::isfinite(Length) || Length <= std::numeric_limits<float>::epsilon())
			return std::unexpected(MakeError(AudioErrorCode::InvalidDescription, "Listener direction must be non-zero"));
		const glm::vec3 Normalized = Direction / Length;
		ma_engine_listener_set_direction(&m_Impl->Engine, 0, Normalized.x, Normalized.y, Normalized.z);
		return {};
	}
}
