#include "Audio/AudioEngine.h"

#include <miniaudio.h>

#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace PulseForge
{
	namespace
	{
		constexpr size_t MaxAudioClipBytes = 256u * 1024u * 1024u;
		constexpr size_t AudioBusCount = 3;
		constexpr ma_uint32 OfflineSampleRate = 48000;
		constexpr ma_uint32 OfflineChannels = 2;
		std::atomic<uint64_t> NextDeviceGeneration{ 1 };

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

		std::optional<size_t> GetBusIndex(AudioBus Bus)
		{
			switch (Bus)
			{
				case AudioBus::Music: return 0;
				case AudioBus::Effects: return 1;
				case AudioBus::UI: return 2;
			}
			return std::nullopt;
		}

		AudioDeviceEvent ConvertDeviceEvent(ma_device_notification_type Type)
		{
			switch (Type)
			{
				case ma_device_notification_type_started: return AudioDeviceEvent::Started;
				case ma_device_notification_type_stopped: return AudioDeviceEvent::Stopped;
				case ma_device_notification_type_rerouted: return AudioDeviceEvent::Rerouted;
				case ma_device_notification_type_interruption_began: return AudioDeviceEvent::InterruptionBegan;
				case ma_device_notification_type_interruption_ended: return AudioDeviceEvent::InterruptionEnded;
				case ma_device_notification_type_unlocked: return AudioDeviceEvent::Unlocked;
			}
			return AudioDeviceEvent::Stopped;
		}
	}

	struct AudioEngine::Impl final
	{
		// miniaudio passes this address as ma_device::pUserData to the notification callback.
		// Keeping it first lets that callback reach only the atomic notification slot.
		ma_engine Engine{};
		bool EngineInitialized = false;
		ma_context Context{};
		bool ContextInitialized = false;
		ma_sound_group MasterGroup{};
		bool MasterGroupInitialized = false;
		std::array<ma_sound_group, AudioBusCount> BusGroups{};
		std::array<bool, AudioBusCount> BusGroupsInitialized{};
		std::array<float, AudioBusCount> BusGains{ 1.0f, 1.0f, 1.0f };
		std::array<bool, AudioBusCount> BusMutes{};
		float MasterGain = 1.0f;
		bool MasterMuted = false;
		AudioOutputBackend OutputBackend = AudioOutputBackend::Default;
		uint64_t DeviceGeneration = 0;
		std::thread::id OwnerThread = std::this_thread::get_id();
		std::atomic<int> PendingDeviceEvent{ -1 };
		std::atomic<uint64_t> RetainedClipCount{ 0 };
		std::atomic<uint64_t> RetainedEncodedClipBytes{ 0 };
		std::atomic<uint64_t> LivePlaybackCount{ 0 };

		static void VerifyNotificationLayout()
		{
			static_assert(std::is_standard_layout_v<Impl>);
		}

		static void OnDeviceNotification(const ma_device_notification* Notification) noexcept
		{
			if (!Notification || !Notification->pDevice || !Notification->pDevice->pUserData)
				return;

			auto* Engine = static_cast<ma_engine*>(Notification->pDevice->pUserData);
			auto* Implementation = reinterpret_cast<Impl*>(Engine);
			Implementation->PendingDeviceEvent.store(static_cast<int>(ConvertDeviceEvent(Notification->type)), std::memory_order_release);
		}

		[[nodiscard]] bool IsOwnerThread() const noexcept
		{
			return OwnerThread == std::this_thread::get_id();
		}

		void ApplyMasterGain() noexcept
		{
			if (MasterGroupInitialized)
				ma_sound_group_set_volume(&MasterGroup, MasterMuted ? 0.0f : MasterGain);
		}

		void ApplyBusGain(size_t Index) noexcept
		{
			if (BusGroupsInitialized[Index])
				ma_sound_group_set_volume(&BusGroups[Index], BusMutes[Index] ? 0.0f : BusGains[Index]);
		}

		~Impl()
		{
			for (size_t Index = BusGroupsInitialized.size(); Index > 0; --Index)
			{
				if (BusGroupsInitialized[Index - 1])
					ma_sound_group_uninit(&BusGroups[Index - 1]);
			}
			if (MasterGroupInitialized)
				ma_sound_group_uninit(&MasterGroup);
			if (EngineInitialized)
				ma_engine_uninit(&Engine);
			if (ContextInitialized)
				ma_context_uninit(&Context);
		}
	};

	struct AudioClip::Impl final
	{
		std::string DebugName;
		std::vector<std::byte> EncodedData;
		std::weak_ptr<AudioEngine::Impl> Owner;
		bool Counted = false;

		~Impl()
		{
			if (!Counted)
				return;
			if (const auto Engine = Owner.lock())
			{
				Engine->RetainedClipCount.fetch_sub(1, std::memory_order_relaxed);
				Engine->RetainedEncodedClipBytes.fetch_sub(static_cast<uint64_t>(EncodedData.size()), std::memory_order_relaxed);
			}
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
		bool Counted = false;

		~Impl()
		{
			if (SoundInitialized)
			{
				(void)ma_sound_stop(&Sound);
				ma_sound_uninit(&Sound);
			}
			if (DecoderInitialized)
				(void)ma_decoder_uninit(&Decoder);
			if (Counted && Device)
				Device->LivePlaybackCount.fetch_sub(1, std::memory_order_relaxed);
		}

		[[nodiscard]] bool IsOwnerThread() const noexcept
		{
			return Device && Device->IsOwnerThread();
		}
	};

	std::expected<void, AudioError> AudioPlaybackDesc::Validate() const
	{
		if (!std::isfinite(Volume) || Volume < 0.0f || Volume > 1.0f)
			return std::unexpected(MakeError(AudioErrorCode::InvalidDescription, "Audio volume must be finite and between zero and one"));
		if (!GetBusIndex(Bus))
			return std::unexpected(MakeError(AudioErrorCode::InvalidBus, "Audio playback bus is not supported"));
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
		if (!m_Impl->IsOwnerThread())
			return std::unexpected(MakeError(AudioErrorCode::WrongThread, "Audio playback controls must run on the owning thread"));
		const ma_result Result = ma_sound_stop(&m_Impl->Sound);
		if (Result != MA_SUCCESS)
			return std::unexpected(MakeError(AudioErrorCode::PlaybackControlFailed, MiniaudioError("Pausing audio playback", Result)));
		return {};
	}

	std::expected<void, AudioError> AudioPlayback::Resume()
	{
		if (!IsValid())
			return std::unexpected(MakeError(AudioErrorCode::InvalidPlayback, "Cannot resume an invalid audio playback"));
		if (!m_Impl->IsOwnerThread())
			return std::unexpected(MakeError(AudioErrorCode::WrongThread, "Audio playback controls must run on the owning thread"));
		const ma_result Result = ma_sound_start(&m_Impl->Sound);
		if (Result != MA_SUCCESS)
			return std::unexpected(MakeError(AudioErrorCode::PlaybackControlFailed, MiniaudioError("Resuming audio playback", Result)));
		return {};
	}

	std::expected<void, AudioError> AudioPlayback::Stop()
	{
		if (!IsValid())
			return std::unexpected(MakeError(AudioErrorCode::InvalidPlayback, "Cannot stop an invalid audio playback"));
		if (!m_Impl->IsOwnerThread())
			return std::unexpected(MakeError(AudioErrorCode::WrongThread, "Audio playback controls must run on the owning thread"));
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
		if (!m_Impl->IsOwnerThread())
			return std::unexpected(MakeError(AudioErrorCode::WrongThread, "Audio playback controls must run on the owning thread"));
		if (!std::isfinite(Volume) || Volume < 0.0f || Volume > 1.0f)
			return std::unexpected(MakeError(AudioErrorCode::InvalidDescription, "Audio volume must be finite and between zero and one"));
		ma_sound_set_volume(&m_Impl->Sound, Volume);
		return {};
	}

	std::expected<void, AudioError> AudioPlayback::SetLooping(bool Looping)
	{
		if (!IsValid())
			return std::unexpected(MakeError(AudioErrorCode::InvalidPlayback, "Cannot update an invalid audio playback"));
		if (!m_Impl->IsOwnerThread())
			return std::unexpected(MakeError(AudioErrorCode::WrongThread, "Audio playback controls must run on the owning thread"));
		ma_sound_set_looping(&m_Impl->Sound, Looping ? MA_TRUE : MA_FALSE);
		return {};
	}

	std::expected<void, AudioError> AudioPlayback::SetSpatialized(bool Spatialized)
	{
		if (!IsValid())
			return std::unexpected(MakeError(AudioErrorCode::InvalidPlayback, "Cannot update an invalid audio playback"));
		if (!m_Impl->IsOwnerThread())
			return std::unexpected(MakeError(AudioErrorCode::WrongThread, "Audio playback controls must run on the owning thread"));
		ma_sound_set_spatialization_enabled(&m_Impl->Sound, Spatialized ? MA_TRUE : MA_FALSE);
		return {};
	}

	std::expected<void, AudioError> AudioPlayback::SetPosition(const glm::vec3& Position)
	{
		if (!IsValid())
			return std::unexpected(MakeError(AudioErrorCode::InvalidPlayback, "Cannot update an invalid audio playback"));
		if (!m_Impl->IsOwnerThread())
			return std::unexpected(MakeError(AudioErrorCode::WrongThread, "Audio playback controls must run on the owning thread"));
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
		if (Description.OutputBackend != AudioOutputBackend::Default &&
			Description.OutputBackend != AudioOutputBackend::Null &&
			Description.OutputBackend != AudioOutputBackend::Offline)
			return std::unexpected(MakeError(AudioErrorCode::InvalidDescription, "Audio output backend is not supported"));

		try
		{
			Impl::VerifyNotificationLayout();
			auto Implementation = std::make_shared<Impl>();
			Implementation->OutputBackend = Description.OutputBackend;
			Implementation->DeviceGeneration = NextDeviceGeneration.fetch_add(1, std::memory_order_relaxed);
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
			if (Description.OutputBackend == AudioOutputBackend::Offline)
			{
				EngineConfiguration.noDevice = MA_TRUE;
				EngineConfiguration.channels = OfflineChannels;
				EngineConfiguration.sampleRate = OfflineSampleRate;
			}
			else
			{
				EngineConfiguration.notificationCallback = &Impl::OnDeviceNotification;
			}
			if (const ma_result Result = ma_engine_init(&EngineConfiguration, &Implementation->Engine);
				Result != MA_SUCCESS)
			{
				return std::unexpected(MakeError(
					AudioErrorCode::DeviceInitializationFailed,
					MiniaudioError("Initializing the audio engine", Result)));
			}
			Implementation->EngineInitialized = true;

			if (const ma_result Result = ma_sound_group_init(
					&Implementation->Engine,
					MA_SOUND_FLAG_NO_SPATIALIZATION,
					nullptr,
					&Implementation->MasterGroup);
				Result != MA_SUCCESS)
			{
				return std::unexpected(MakeError(
					AudioErrorCode::DeviceInitializationFailed,
					MiniaudioError("Creating the master audio group", Result)));
			}
			Implementation->MasterGroupInitialized = true;
			for (size_t Index = 0; Index < AudioBusCount; ++Index)
			{
				if (const ma_result Result = ma_sound_group_init(
						&Implementation->Engine,
						MA_SOUND_FLAG_NO_SPATIALIZATION,
						&Implementation->MasterGroup,
						&Implementation->BusGroups[Index]);
					Result != MA_SUCCESS)
				{
					return std::unexpected(MakeError(
						AudioErrorCode::DeviceInitializationFailed,
						MiniaudioError("Creating an audio bus group", Result)));
				}
				Implementation->BusGroupsInitialized[Index] = true;
				Implementation->ApplyBusGain(Index);
			}
			Implementation->ApplyMasterGain();
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

	uint64_t AudioEngine::GetDeviceGeneration() const noexcept
	{
		return m_Impl ? m_Impl->DeviceGeneration : 0;
	}

	AudioEngineMemoryStats AudioEngine::GetMemoryStats() const noexcept
	{
		if (!m_Impl)
			return {};

		const uint64_t PlaybackCount = m_Impl->LivePlaybackCount.load(std::memory_order_relaxed);
		return {
			.RetainedClipCount = m_Impl->RetainedClipCount.load(std::memory_order_relaxed),
			.RetainedEncodedClipBytes = m_Impl->RetainedEncodedClipBytes.load(std::memory_order_relaxed),
			.LivePlaybackCount = PlaybackCount,
			.KnownPlaybackObjectBytes = PlaybackCount * static_cast<uint64_t>(sizeof(AudioPlayback::Impl)),
			.KnownDecoderObjectBytes = PlaybackCount * static_cast<uint64_t>(sizeof(ma_decoder)),
			.BackendAllocatedBytes = std::nullopt
		};
	}

	std::expected<void, AudioError> AudioEngine::SetMasterGain(float Gain)
	{
		if (!IsInitialized())
			return std::unexpected(MakeError(AudioErrorCode::DeviceInitializationFailed, "Audio engine is not initialized"));
		if (!m_Impl->IsOwnerThread())
			return std::unexpected(MakeError(AudioErrorCode::WrongThread, "Audio mix changes must run on the owning thread"));
		if (!std::isfinite(Gain) || Gain < 0.0f || Gain > 1.0f)
			return std::unexpected(MakeError(AudioErrorCode::InvalidDescription, "Master gain must be finite and between zero and one"));

		m_Impl->MasterGain = Gain;
		m_Impl->ApplyMasterGain();
		return {};
	}

	std::expected<void, AudioError> AudioEngine::SetBusGain(AudioBus Bus, float Gain)
	{
		if (!IsInitialized())
			return std::unexpected(MakeError(AudioErrorCode::DeviceInitializationFailed, "Audio engine is not initialized"));
		if (!m_Impl->IsOwnerThread())
			return std::unexpected(MakeError(AudioErrorCode::WrongThread, "Audio mix changes must run on the owning thread"));
		const auto Index = GetBusIndex(Bus);
		if (!Index)
			return std::unexpected(MakeError(AudioErrorCode::InvalidBus, "Audio bus is not supported"));
		if (!std::isfinite(Gain) || Gain < 0.0f || Gain > 1.0f)
			return std::unexpected(MakeError(AudioErrorCode::InvalidDescription, "Bus gain must be finite and between zero and one"));

		m_Impl->BusGains[*Index] = Gain;
		m_Impl->ApplyBusGain(*Index);
		return {};
	}

	std::expected<void, AudioError> AudioEngine::SetMasterMuted(bool Muted)
	{
		if (!IsInitialized())
			return std::unexpected(MakeError(AudioErrorCode::DeviceInitializationFailed, "Audio engine is not initialized"));
		if (!m_Impl->IsOwnerThread())
			return std::unexpected(MakeError(AudioErrorCode::WrongThread, "Audio mix changes must run on the owning thread"));
		m_Impl->MasterMuted = Muted;
		m_Impl->ApplyMasterGain();
		return {};
	}

	std::expected<void, AudioError> AudioEngine::SetBusMuted(AudioBus Bus, bool Muted)
	{
		if (!IsInitialized())
			return std::unexpected(MakeError(AudioErrorCode::DeviceInitializationFailed, "Audio engine is not initialized"));
		if (!m_Impl->IsOwnerThread())
			return std::unexpected(MakeError(AudioErrorCode::WrongThread, "Audio mix changes must run on the owning thread"));
		const auto Index = GetBusIndex(Bus);
		if (!Index)
			return std::unexpected(MakeError(AudioErrorCode::InvalidBus, "Audio bus is not supported"));
		m_Impl->BusMutes[*Index] = Muted;
		m_Impl->ApplyBusGain(*Index);
		return {};
	}

	std::expected<AudioBusMix, AudioError> AudioEngine::GetBusMix(AudioBus Bus) const
	{
		if (!IsInitialized())
			return std::unexpected(MakeError(AudioErrorCode::DeviceInitializationFailed, "Audio engine is not initialized"));
		if (!m_Impl->IsOwnerThread())
			return std::unexpected(MakeError(AudioErrorCode::WrongThread, "Audio mix state must be read on the owning thread"));
		const auto Index = GetBusIndex(Bus);
		if (!Index)
			return std::unexpected(MakeError(AudioErrorCode::InvalidBus, "Audio bus is not supported"));

		const float Master = m_Impl->MasterMuted ? 0.0f : m_Impl->MasterGain;
		const float Category = m_Impl->BusMutes[*Index] ? 0.0f : m_Impl->BusGains[*Index];
		return AudioBusMix{
			.MasterGain = m_Impl->MasterGain,
			.BusGain = m_Impl->BusGains[*Index],
			.MasterMuted = m_Impl->MasterMuted,
			.BusMuted = m_Impl->BusMutes[*Index],
			.EffectiveGain = Master * Category
		};
	}

	std::expected<size_t, AudioError> AudioEngine::RenderOfflineFrames(std::span<float> InterleavedSamples)
	{
		if (!IsInitialized())
			return std::unexpected(MakeError(AudioErrorCode::DeviceInitializationFailed, "Audio engine is not initialized"));
		if (!m_Impl->IsOwnerThread())
			return std::unexpected(MakeError(AudioErrorCode::WrongThread, "Offline audio rendering must run on the owning thread"));
		if (m_Impl->OutputBackend != AudioOutputBackend::Offline)
			return std::unexpected(MakeError(
				AudioErrorCode::OfflineRenderingUnavailable,
				"PCM frame rendering is available only from the explicit Offline backend"));
		if (InterleavedSamples.size() % OfflineChannels != 0)
			return std::unexpected(MakeError(
				AudioErrorCode::InvalidDescription,
				"Offline output must contain an integral number of stereo frames"));

		const size_t RequestedFrames = InterleavedSamples.size() / OfflineChannels;
		if (RequestedFrames == 0)
			return size_t{ 0 };
		ma_uint64 FramesRead = 0;
		const ma_result Result = ma_engine_read_pcm_frames(
			&m_Impl->Engine,
			InterleavedSamples.data(),
			static_cast<ma_uint64>(RequestedFrames),
			&FramesRead);
		if (Result != MA_SUCCESS)
			return std::unexpected(MakeError(
				AudioErrorCode::PlaybackControlFailed,
				MiniaudioError("Rendering offline audio frames", Result)));
		return static_cast<size_t>(FramesRead);
	}

	std::optional<AudioDeviceEvent> AudioEngine::ConsumeDeviceEvent() noexcept
	{
		if (!m_Impl)
			return std::nullopt;
		const int Event = m_Impl->PendingDeviceEvent.exchange(-1, std::memory_order_acq_rel);
		if (Event < 0 || Event > static_cast<int>(AudioDeviceEvent::Unlocked))
			return std::nullopt;
		return static_cast<AudioDeviceEvent>(Event);
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
			Clip->Owner = m_Impl;
			m_Impl->RetainedClipCount.fetch_add(1, std::memory_order_relaxed);
			m_Impl->RetainedEncodedClipBytes.fetch_add(static_cast<uint64_t>(Clip->EncodedData.size()), std::memory_order_relaxed);
			Clip->Counted = true;
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
		if (!m_Impl->IsOwnerThread())
			return std::unexpected(MakeError(AudioErrorCode::WrongThread, "Creating playback must run on the owning thread"));
		if (!Clip.m_Impl)
			return std::unexpected(MakeError(AudioErrorCode::InvalidClip, "Cannot play an empty audio clip handle"));
		if (auto Validation = Description.Validate(); !Validation)
			return std::unexpected(Validation.error());
		const auto BusIndex = GetBusIndex(Description.Bus);
		if (!BusIndex)
			return std::unexpected(MakeError(AudioErrorCode::InvalidBus, "Audio playback bus is not supported"));

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
					&m_Impl->BusGroups[*BusIndex],
					&Playback->Sound);
				Result != MA_SUCCESS)
			{
				return std::unexpected(MakeError(
					AudioErrorCode::PlaybackInitializationFailed,
					MiniaudioError("Creating audio playback", Result)));
			}
			Playback->SoundInitialized = true;
			m_Impl->LivePlaybackCount.fetch_add(1, std::memory_order_relaxed);
			Playback->Counted = true;
			ma_sound_set_volume(&Playback->Sound, Description.Volume);
			ma_sound_set_looping(&Playback->Sound, Description.Looping ? MA_TRUE : MA_FALSE);
			if (Description.Spatialized)
				ma_sound_set_position(&Playback->Sound, Description.Position.x, Description.Position.y, Description.Position.z);
			if (!Description.StartPaused)
			{
				if (const ma_result Result = ma_sound_start(&Playback->Sound); Result != MA_SUCCESS)
				{
					return std::unexpected(MakeError(
						AudioErrorCode::PlaybackStartFailed,
						MiniaudioError("Starting audio playback", Result)));
				}
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
		if (!m_Impl->IsOwnerThread())
			return std::unexpected(MakeError(AudioErrorCode::WrongThread, "Audio listener updates must run on the owning thread"));
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
		if (!m_Impl->IsOwnerThread())
			return std::unexpected(MakeError(AudioErrorCode::WrongThread, "Audio listener updates must run on the owning thread"));
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
