#include "Audio/AudioSession.h"

#include "Assets/AudioAssetCache.h"
#include "Core/Diagnostics.h"
#include "Core/PlatformServices.h"
#include "Runtime/CookedContentProvider.h"
#if !defined(PF_COOKED_RUNTIME_ONLY)
#include "Assets/Project.h"
#endif

#include <nlohmann/json.hpp>

#include <atomic>
#include <fstream>
#include <iterator>
#include <limits>
#include <system_error>
#include <thread>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace PulseForge
{
	namespace
	{
		using Json = nlohmann::ordered_json;
		constexpr size_t MaximumAudioSettingsBytes = 64u * 1024u;

		std::expected<AudioEngine, AudioError> CreateAudioEngine(
			const AudioEngineDesc& Description,
			const std::function<std::expected<AudioEngine, AudioError>(AudioOutputBackend)>& Factory,
			AudioOutputBackend Backend)
		{
			try
			{
				if (Factory)
					return Factory(Backend);
				AudioEngineDesc Requested = Description;
				Requested.OutputBackend = Backend;
				return AudioEngine::Create(Requested);
			}
			catch (const std::exception& Exception)
			{
				return std::unexpected(AudioError{ AudioErrorCode::DeviceInitializationFailed,
					std::string("Audio engine factory threw: ") + Exception.what() });
			}
			catch (...)
			{
				return std::unexpected(AudioError{ AudioErrorCode::DeviceInitializationFailed, "Audio engine factory threw unexpectedly" });
			}
		}

		class ScopedOwnedTemporaryPath final
		{
		public:
			explicit ScopedOwnedTemporaryPath(const std::filesystem::path& Value) noexcept
				: Path(Value)
			{
			}
			~ScopedOwnedTemporaryPath()
			{
				if (!Owns) return;
				std::error_code Error;
				std::filesystem::remove(Path, Error);
			}

			void TakeOwnership() noexcept { Owns = true; }
			void Release() noexcept { Owns = false; }

		private:
			const std::filesystem::path& Path;
			bool Owns = false;
		};

		std::expected<void, std::string> WriteExclusiveSettingsFile(
			const std::filesystem::path& Path,
			std::string_view Contents)
		{
			ScopedOwnedTemporaryPath Cleanup(Path);
#if defined(_WIN32)
			HANDLE File = CreateFileW(Path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (File == INVALID_HANDLE_VALUE)
				return std::unexpected("Could not exclusively create temporary audio settings file: " +
					std::error_code(static_cast<int>(GetLastError()), std::system_category()).message());
			Cleanup.TakeOwnership();
			size_t Offset = 0;
			while (Offset < Contents.size())
			{
				const size_t Remaining = Contents.size() - Offset;
				const size_t MaximumWrite = static_cast<size_t>(std::numeric_limits<DWORD>::max());
				const DWORD ChunkSize = static_cast<DWORD>(Remaining <= MaximumWrite ? Remaining : MaximumWrite);
				DWORD Written = 0;
				if (!WriteFile(File, Contents.data() + Offset, ChunkSize, &Written, nullptr) || Written == 0)
				{
					const std::string Message = "Could not write temporary audio settings file: " +
						std::error_code(static_cast<int>(GetLastError()), std::system_category()).message();
					CloseHandle(File);
					return std::unexpected(Message);
				}
				Offset += Written;
			}
			if (!CloseHandle(File))
				return std::unexpected("Could not close temporary audio settings file: " +
					std::error_code(static_cast<int>(GetLastError()), std::system_category()).message());
#else
			const int File = ::open(Path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, S_IRUSR | S_IWUSR);
			if (File < 0)
				return std::unexpected("Could not exclusively create temporary audio settings file: " +
					std::error_code(errno, std::generic_category()).message());
			Cleanup.TakeOwnership();
			size_t Offset = 0;
			while (Offset < Contents.size())
			{
				const ssize_t Written = ::write(File, Contents.data() + Offset, Contents.size() - Offset);
				if (Written < 0 && errno == EINTR) continue;
				if (Written <= 0)
				{
					const std::string Message = "Could not write temporary audio settings file: " +
						std::error_code(errno, std::generic_category()).message();
					::close(File);
					return std::unexpected(Message);
				}
				Offset += static_cast<size_t>(Written);
			}
			if (::close(File) != 0)
				return std::unexpected("Could not close temporary audio settings file: " +
					std::error_code(errno, std::generic_category()).message());
#endif
			Cleanup.Release();
			return {};
		}

		AudioSessionError MakeError(AudioSessionErrorCode Code, std::string Message)
		{
			return { Code, std::move(Message) };
		}

		uint64_t NextSessionGeneration() noexcept
		{
			static std::atomic<uint64_t> Generation = 0;
			uint64_t Value = Generation.fetch_add(1, std::memory_order_relaxed) + 1;
			if (Value == 0) Value = Generation.fetch_add(1, std::memory_order_relaxed) + 1;
			return Value;
		}

		std::string PathForMessage(const std::filesystem::path& Path)
		{
			const std::u8string Encoded = Path.u8string();
			return { reinterpret_cast<const char*>(Encoded.data()), Encoded.size() };
		}

		bool IsValidBus(AudioBus Bus) noexcept
		{
			return Bus == AudioBus::Music || Bus == AudioBus::Effects || Bus == AudioBus::UI;
		}

		std::string_view BusName(AudioBus Bus) noexcept
		{
			switch (Bus)
			{
				case AudioBus::Music: return "music";
				case AudioBus::Effects: return "effects";
				case AudioBus::UI: return "ui";
			}
			return {};
		}

		std::expected<float, AudioSessionError> ReadGain(const Json& Object, std::string_view Key, bool Required)
		{
			const auto Value = Object.find(Key);
			if (Value == Object.end())
			{
				if (Required)
					return std::unexpected(MakeError(AudioSessionErrorCode::SettingsReadFailed,
						"User audio settings are missing gain '" + std::string(Key) + "'"));
				return 1.0f;
			}
			if (!Value->is_number())
				return std::unexpected(MakeError(AudioSessionErrorCode::SettingsReadFailed,
					"User audio setting '" + std::string(Key) + "' must be a number"));
			const float Gain = Value->get<float>();
			if (!std::isfinite(Gain) || Gain < 0.0f || Gain > 1.0f)
				return std::unexpected(MakeError(AudioSessionErrorCode::SettingsReadFailed,
					"User audio gain '" + std::string(Key) + "' must be finite and between zero and one"));
			return Gain;
		}

		std::expected<bool, AudioSessionError> ReadMute(const Json& Object, std::string_view Key)
		{
			const auto Value = Object.find(Key);
			if (Value == Object.end() || !Value->is_boolean())
				return std::unexpected(MakeError(AudioSessionErrorCode::SettingsReadFailed,
					"User audio settings require boolean mute field '" + std::string(Key) + "'"));
			return Value->get<bool>();
		}
	}

	struct AudioSession::Impl final
	{
		std::unique_ptr<AudioEngine> Engine;
		std::unique_ptr<AudioAssetCache> Assets;
		UUID ProjectID;
		AudioMixGains AuthoredDefaults;
		AudioUserMixOverrides UserOverrides;
		AudioMixState MixState;
		std::filesystem::path SettingsPath;
		std::string DeviceDiagnostic;
		std::string SettingsDiagnostic;
		DiagnosticRecorder* Diagnostics = nullptr;
		AudioEngineDesc EngineDescription;
		std::function<std::expected<AudioEngine, AudioError>(AudioOutputBackend)> EngineFactory;
		AudioDeviceState DeviceState = AudioDeviceState::Unavailable;
		uint64_t Generation = 0;
		std::thread::id OwnerThread;
		bool SilentFallback = false;
		bool SettingsDirty = false;

		using CookedReader = std::function<std::expected<std::vector<std::byte>, AudioAssetError>(const AssetID&)>;

		std::expected<AudioEngine, AudioError> CreateOutput(AudioOutputBackend Backend) const
		{
			return CreateAudioEngine(EngineDescription, EngineFactory, Backend);
		}

		static std::expected<std::unique_ptr<Impl>, AudioSessionError> Create(
			const UUID& ProjectIdentifier,
			AudioMixGains Defaults,
			AudioSessionDesc Description,
			const std::filesystem::path* ProjectRoot,
			const AssetRegistry* Registry,
			CookedReader Reader)
		{
			if (ProjectIdentifier.IsNil())
				return std::unexpected(MakeError(AudioSessionErrorCode::InvalidDescription,
					"Audio session requires a non-nil project identity"));
			if (!Defaults.IsValid())
				return std::unexpected(MakeError(AudioSessionErrorCode::InvalidMixSettings,
					"Authored project audio gains must be finite and between zero and one"));
			if ((!ProjectRoot || !Registry) && !Reader)
				return std::unexpected(MakeError(AudioSessionErrorCode::AssetProviderUnavailable,
					"Audio session requires a managed project registry or cooked audio provider"));

			auto Factory = Description.EngineFactory;
			const AudioEngineDesc EngineDescription = Description.Engine;
			auto CreateOutput = [&Factory, EngineDescription](AudioOutputBackend Backend)
				-> std::expected<AudioEngine, AudioError>
			{
				return CreateAudioEngine(EngineDescription, Factory, Backend);
			};
			auto Device = CreateOutput(Description.Engine.OutputBackend);
			AudioDeviceState InitialState = AudioDeviceState::DefaultActive;
			std::string DeviceError;
			bool SilentFallback = false;
			if (!Device)
			{
				if (Description.Engine.OutputBackend != AudioOutputBackend::Default)
					return std::unexpected(MakeError(AudioSessionErrorCode::EngineInitializationFailed, Device.error().Message));
				DeviceError = "Default audio output is unavailable: " + Device.error().Message +
					". Simulation will continue with silent output; retry explicitly after restoring a device.";
				auto SilentDevice = CreateOutput(AudioOutputBackend::Null);
				if (!SilentDevice)
					return std::unexpected(MakeError(AudioSessionErrorCode::EngineInitializationFailed,
						DeviceError + " Silent fallback initialization also failed: " + SilentDevice.error().Message));
				Device = std::move(SilentDevice);
				InitialState = AudioDeviceState::Unavailable;
				SilentFallback = true;
			}
			else if (Description.Engine.OutputBackend == AudioOutputBackend::Null)
				InitialState = AudioDeviceState::ExplicitNull;
			else if (Description.Engine.OutputBackend == AudioOutputBackend::Offline)
				InitialState = AudioDeviceState::OfflineTest;

			try
			{
				auto Result = std::make_unique<Impl>();
				Result->Engine = std::make_unique<AudioEngine>(std::move(*Device));
				if (Reader)
					Result->Assets = std::make_unique<AudioAssetCache>(*Result->Engine, std::move(Reader));
	#if !defined(PF_COOKED_RUNTIME_ONLY)
				else
					Result->Assets = std::make_unique<AudioAssetCache>(*Result->Engine, *ProjectRoot, *Registry);
	#else
				else
					return std::unexpected(MakeError(AudioSessionErrorCode::AssetProviderUnavailable,
						"Cooked audio sessions require the cooked audio asset reader"));
	#endif
				Result->ProjectID = ProjectIdentifier;
				Result->AuthoredDefaults = Defaults;
				Result->Diagnostics = Description.Diagnostics;
				Result->EngineDescription = EngineDescription;
				Result->EngineFactory = std::move(Factory);
				Result->DeviceState = InitialState;
				Result->DeviceDiagnostic = std::move(DeviceError);
				Result->SilentFallback = SilentFallback;
				Result->Generation = NextSessionGeneration();
				Result->OwnerThread = std::this_thread::get_id();
				Result->RecordDeviceState();

				auto UserDataDirectory = Description.UserDataDirectory.empty()
					? Platform::GetUserDataDirectory()
					: Platform::PathResult(Description.UserDataDirectory);
				if (!UserDataDirectory || UserDataDirectory->empty() || !UserDataDirectory->is_absolute())
					Result->SettingsDiagnostic = UserDataDirectory
						? "User audio settings require an absolute user-data directory"
						: "User audio settings are unavailable: " + UserDataDirectory.error().Message;
				else
				{
					Result->SettingsPath = *UserDataDirectory / "PulseForge" / "AudioSettings" /
						(Result->ProjectID.ToString() + ".json");
					Result->LoadUserSettings();
				}

				if (auto Applied = Result->ApplyMix(); !Applied)
					return std::unexpected(Applied.error());
				return Result;
			}
			catch (const std::exception& Exception)
			{
				return std::unexpected(MakeError(AudioSessionErrorCode::EngineInitializationFailed,
					std::string("Could not create audio session state: ") + Exception.what()));
			}
		}

		void RecordSettingsWarning() noexcept
		{
			if (Diagnostics && !SettingsDiagnostic.empty())
				(void)Diagnostics->RecordEvent(DiagnosticEventKind::Counter, "Audio.UserSettingsFallback", "AudioSession");
		}

		void RecordDeviceState() noexcept
		{
			if (Diagnostics)
				(void)Diagnostics->RecordEvent(DiagnosticEventKind::Resource, "Audio.DeviceState", "AudioSession", "state",
					static_cast<uint64_t>(DeviceState));
		}

		[[nodiscard]] bool IsOwnerThread() const noexcept
		{
			return OwnerThread == std::this_thread::get_id();
		}

		void LoadUserSettings()
		{
			if (SettingsPath.empty()) return;
			std::error_code Error;
			const bool Exists = std::filesystem::exists(SettingsPath, Error);
			if (Error)
			{
				SettingsDiagnostic = "Could not inspect user audio settings '" + PathForMessage(SettingsPath) + "': " + Error.message();
				RecordSettingsWarning();
				return;
			}
			if (!Exists) return;

			const uintmax_t Size = std::filesystem::file_size(SettingsPath, Error);
			if (Error || Size > MaximumAudioSettingsBytes)
			{
				SettingsDiagnostic = Error
					? "Could not inspect user audio settings: " + Error.message()
					: "User audio settings exceed the 64 KiB limit; the file was left untouched";
				RecordSettingsWarning();
				return;
			}
			std::ifstream Input(SettingsPath, std::ios::binary);
			if (!Input.is_open())
			{
				SettingsDiagnostic = "Could not open user audio settings '" + PathForMessage(SettingsPath) + "'";
				RecordSettingsWarning();
				return;
			}
			std::string Contents(static_cast<size_t>(Size), '\0');
			if (!Contents.empty()) Input.read(Contents.data(), static_cast<std::streamsize>(Contents.size()));
			if (!Input || Input.bad() || static_cast<size_t>(Input.gcount()) != Contents.size())
			{
				SettingsDiagnostic = "Could not read complete user audio settings; the file was left untouched";
				RecordSettingsWarning();
				return;
			}

			try
			{
				const Json Root = Json::parse(Contents);
				if (!Root.is_object() || !Root.contains("version") || !Root["version"].is_number_integer() ||
					Root["version"].get<int64_t>() != 1)
					throw std::runtime_error("User audio settings version is unsupported; the file was left untouched");
				if (Root.size() != 4 || !Root.contains("projectId") || !Root["projectId"].is_string() ||
					Root["projectId"].get<std::string>() != ProjectID.ToString() ||
					!Root.contains("overrides") || !Root["overrides"].is_object() ||
					!Root.contains("mutes") || !Root["mutes"].is_object())
					throw std::runtime_error("User audio settings identity or objects are invalid; the file was left untouched");
				const Json& Overrides = Root["overrides"];
				for (const auto& [Key, Value] : Overrides.items())
				{
					(void)Value;
					if (Key != "master" && Key != "music" && Key != "effects" && Key != "ui")
						throw std::runtime_error("User audio settings contain unknown gain override '" + Key + "'");
				}
				const Json& Mutes = Root["mutes"];
				if (Mutes.size() != 4 || !Mutes.contains("master") || !Mutes.contains("music") ||
					!Mutes.contains("effects") || !Mutes.contains("ui"))
					throw std::runtime_error("User audio mute settings are incomplete");
				auto Master = ReadGain(Overrides, "master", false);
				auto Music = ReadGain(Overrides, "music", false);
				auto Effects = ReadGain(Overrides, "effects", false);
				auto UI = ReadGain(Overrides, "ui", false);
				auto MasterMuted = ReadMute(Mutes, "master");
				auto MusicMuted = ReadMute(Mutes, "music");
				auto EffectsMuted = ReadMute(Mutes, "effects");
				auto UIMuted = ReadMute(Mutes, "ui");
				if (!Master || !Music || !Effects || !UI || !MasterMuted || !MusicMuted || !EffectsMuted || !UIMuted)
					throw std::runtime_error("User audio settings contain an invalid gain or mute value");
				if (Overrides.contains("master")) UserOverrides.Master = *Master;
				if (Overrides.contains("music")) UserOverrides.Music = *Music;
				if (Overrides.contains("effects")) UserOverrides.Effects = *Effects;
				if (Overrides.contains("ui")) UserOverrides.UI = *UI;
				UserOverrides.MasterMuted = *MasterMuted;
				UserOverrides.MusicMuted = *MusicMuted;
				UserOverrides.EffectsMuted = *EffectsMuted;
				UserOverrides.UIMuted = *UIMuted;
			}
			catch (const std::exception& Exception)
			{
				UserOverrides = {};
				SettingsDiagnostic = std::string("User audio settings are malformed: ") + Exception.what();
				RecordSettingsWarning();
			}
		}

		AudioMixState BuildMixState() const noexcept
		{
			AudioMixState Result;
			Result.Gains.Master = UserOverrides.Master.value_or(AuthoredDefaults.Master);
			Result.Gains.Music = UserOverrides.Music.value_or(AuthoredDefaults.Music);
			Result.Gains.Effects = UserOverrides.Effects.value_or(AuthoredDefaults.Effects);
			Result.Gains.UI = UserOverrides.UI.value_or(AuthoredDefaults.UI);
			Result.MasterMuted = UserOverrides.MasterMuted;
			Result.MusicMuted = UserOverrides.MusicMuted;
			Result.EffectsMuted = UserOverrides.EffectsMuted;
			Result.UIMuted = UserOverrides.UIMuted;
			return Result;
		}

		std::expected<void, AudioSessionError> ApplyMix()
		{
			if (!IsOwnerThread())
				return std::unexpected(MakeError(AudioSessionErrorCode::WrongThread,
					"Audio mix changes must run on the owning session thread"));
			MixState = BuildMixState();
			if (auto Result = Engine->SetMasterGain(MixState.Gains.Master); !Result)
				return std::unexpected(MakeError(AudioSessionErrorCode::InvalidMixSettings, Result.error().Message));
			if (auto Result = Engine->SetBusGain(AudioBus::Music, MixState.Gains.Music); !Result)
				return std::unexpected(MakeError(AudioSessionErrorCode::InvalidMixSettings, Result.error().Message));
			if (auto Result = Engine->SetBusGain(AudioBus::Effects, MixState.Gains.Effects); !Result)
				return std::unexpected(MakeError(AudioSessionErrorCode::InvalidMixSettings, Result.error().Message));
			if (auto Result = Engine->SetBusGain(AudioBus::UI, MixState.Gains.UI); !Result)
				return std::unexpected(MakeError(AudioSessionErrorCode::InvalidMixSettings, Result.error().Message));
			if (auto Result = Engine->SetMasterMuted(MixState.MasterMuted); !Result)
				return std::unexpected(MakeError(AudioSessionErrorCode::InvalidMixSettings, Result.error().Message));
			if (auto Result = Engine->SetBusMuted(AudioBus::Music, MixState.MusicMuted); !Result)
				return std::unexpected(MakeError(AudioSessionErrorCode::InvalidMixSettings, Result.error().Message));
			if (auto Result = Engine->SetBusMuted(AudioBus::Effects, MixState.EffectsMuted); !Result)
				return std::unexpected(MakeError(AudioSessionErrorCode::InvalidMixSettings, Result.error().Message));
			if (auto Result = Engine->SetBusMuted(AudioBus::UI, MixState.UIMuted); !Result)
				return std::unexpected(MakeError(AudioSessionErrorCode::InvalidMixSettings, Result.error().Message));
			return {};
		}
	};

	AudioSession::AudioSession(std::unique_ptr<Impl> Implementation)
		: m_Impl(std::move(Implementation))
	{
	}

	AudioSession::~AudioSession() = default;

#if !defined(PF_COOKED_RUNTIME_ONLY)
	std::expected<std::unique_ptr<AudioSession>, AudioSessionError> AudioSession::CreateForProject(
		const Project& SourceProject,
		AudioSessionDesc Description)
	{
		auto Result = Impl::Create(SourceProject.GetDescription().ID, SourceProject.GetDescription().AudioMixDefaults,
			std::move(Description), &SourceProject.GetRootPath(), &SourceProject.GetAssetRegistry(), {});
		if (!Result) return std::unexpected(Result.error());
		return std::unique_ptr<AudioSession>(new AudioSession(std::move(*Result)));
	}
#endif

	std::expected<std::unique_ptr<AudioSession>, AudioSessionError> AudioSession::CreateForCooked(
		std::shared_ptr<const CookedContentProvider> CookedAssets,
		AudioMixGains AuthoredDefaults,
		AudioSessionDesc Description)
	{
		if (!CookedAssets)
			return std::unexpected(MakeError(AudioSessionErrorCode::AssetProviderUnavailable,
				"Cooked audio session requires a cooked content provider"));
		const UUID ProjectID = CookedAssets->GetProjectID();
		auto Reader = [Provider = std::move(CookedAssets)](const AssetID& Asset)
			-> std::expected<std::vector<std::byte>, AudioAssetError>
		{
			auto Bytes = Provider->ReadAudio(Asset);
			if (!Bytes)
				return std::unexpected(AudioAssetError{ .Code = AudioAssetErrorCode::FileReadFailed,
					.Asset = Asset, .Message = Bytes.error().Message });
			return std::move(*Bytes);
		};
		auto Result = Impl::Create(ProjectID, AuthoredDefaults, std::move(Description), nullptr, nullptr, std::move(Reader));
		if (!Result) return std::unexpected(Result.error());
		return std::unique_ptr<AudioSession>(new AudioSession(std::move(*Result)));
	}

	AudioEngine& AudioSession::GetEngine() noexcept { return *m_Impl->Engine; }
	AudioAssetCache& AudioSession::GetAssets() noexcept { return *m_Impl->Assets; }
	uint64_t AudioSession::GetGeneration() const noexcept { return m_Impl ? m_Impl->Generation : 0; }
	const UUID& AudioSession::GetProjectID() const noexcept { return m_Impl->ProjectID; }
	AudioDeviceState AudioSession::GetDeviceState() const noexcept { return m_Impl->DeviceState; }
	std::string_view AudioSession::GetDeviceDiagnostic() const noexcept { return m_Impl->DeviceDiagnostic; }
	std::string_view AudioSession::GetSettingsDiagnostic() const noexcept { return m_Impl->SettingsDiagnostic; }
	const std::filesystem::path& AudioSession::GetSettingsPath() const noexcept { return m_Impl->SettingsPath; }
	AudioMixState AudioSession::GetMixState() const noexcept { return m_Impl->MixState; }
	bool AudioSession::HasUnsavedUserSettings() const noexcept { return m_Impl->SettingsDirty; }

	void AudioSession::PollDeviceState() noexcept
	{
		if (!m_Impl || std::this_thread::get_id() != m_Impl->OwnerThread || m_Impl->DeviceState == AudioDeviceState::ExplicitNull ||
			m_Impl->DeviceState == AudioDeviceState::OfflineTest || m_Impl->SilentFallback)
			return;
		const auto Event = m_Impl->Engine->ConsumeDeviceEvent();
		if (!Event) return;
		const AudioDeviceState PreviousState = m_Impl->DeviceState;
		switch (*Event)
		{
			case AudioDeviceEvent::Stopped:
			case AudioDeviceEvent::InterruptionBegan:
				m_Impl->DeviceState = AudioDeviceState::Unavailable;
				m_Impl->DeviceDiagnostic = "Audio output reported an interruption; retry output explicitly to rebuild playback.";
				break;
			case AudioDeviceEvent::Started:
			case AudioDeviceEvent::InterruptionEnded:
				m_Impl->DeviceState = AudioDeviceState::DefaultActive;
				m_Impl->DeviceDiagnostic.clear();
				break;
			case AudioDeviceEvent::Rerouted:
				m_Impl->DeviceState = AudioDeviceState::DefaultActive;
				m_Impl->DeviceDiagnostic = "Audio output route changed; active voices remain owned by the current engine.";
				break;
			case AudioDeviceEvent::Unlocked:
				m_Impl->DeviceState = AudioDeviceState::Unavailable;
				m_Impl->DeviceDiagnostic = "Audio output was unlocked by the operating system; retry output explicitly if playback stopped.";
				break;
		}
		if (m_Impl->DeviceState != PreviousState)
			m_Impl->RecordDeviceState();
		else if (*Event == AudioDeviceEvent::Rerouted && m_Impl->Diagnostics)
			(void)m_Impl->Diagnostics->RecordEvent(DiagnosticEventKind::Counter, "Audio.DeviceRerouted", "AudioSession");
	}

	std::expected<void, AudioSessionError> AudioSession::RetryDefaultOutput()
	{
		if (!m_Impl || std::this_thread::get_id() != m_Impl->OwnerThread)
			return std::unexpected(MakeError(AudioSessionErrorCode::DeviceRecoveryFailed,
				"Audio output recovery must run on the owning session thread"));
		if (m_Impl->DeviceState == AudioDeviceState::OfflineTest)
			return std::unexpected(MakeError(AudioSessionErrorCode::DeviceRecoveryFailed,
				"Offline test audio sessions do not have a physical output to recover"));
		if (m_Impl->DeviceState == AudioDeviceState::ExplicitNull)
			return std::unexpected(MakeError(AudioSessionErrorCode::DeviceRecoveryFailed,
				"This audio session explicitly requested Null output and will not switch to a physical device"));
		m_Impl->DeviceState = AudioDeviceState::Recovering;
		m_Impl->RecordDeviceState();
		auto Replacement = m_Impl->CreateOutput(AudioOutputBackend::Default);
		if (!Replacement)
		{
			m_Impl->DeviceState = AudioDeviceState::RecoveryFailed;
			m_Impl->DeviceDiagnostic = "Audio output retry failed: " + Replacement.error().Message;
			m_Impl->RecordDeviceState();
			if (m_Impl->Diagnostics)
				(void)m_Impl->Diagnostics->RecordEvent(DiagnosticEventKind::Counter, "Audio.DeviceRecoveryFailed", "AudioSession");
			return std::unexpected(MakeError(AudioSessionErrorCode::DeviceRecoveryFailed, m_Impl->DeviceDiagnostic));
		}
		*m_Impl->Engine = std::move(*Replacement);
		m_Impl->SilentFallback = false;
		m_Impl->DeviceState = AudioDeviceState::DefaultActive;
		m_Impl->DeviceDiagnostic.clear();
		m_Impl->Assets->Clear();
		if (auto Applied = m_Impl->ApplyMix(); !Applied)
		{
			m_Impl->DeviceState = AudioDeviceState::RecoveryFailed;
			m_Impl->DeviceDiagnostic = Applied.error().Message;
			m_Impl->RecordDeviceState();
			return std::unexpected(Applied.error());
		}
		if (m_Impl->Diagnostics)
			(void)m_Impl->Diagnostics->RecordEvent(DiagnosticEventKind::Counter, "Audio.DeviceRecovered", "AudioSession");
		m_Impl->RecordDeviceState();
		return {};
	}

	std::expected<void, AudioSessionError> AudioSession::SetUserMasterGain(float Gain)
	{
		if (!m_Impl || !m_Impl->IsOwnerThread())
			return std::unexpected(MakeError(AudioSessionErrorCode::WrongThread,
				"Audio mix changes must run on the owning session thread"));
		if (!std::isfinite(Gain) || Gain < 0.0f || Gain > 1.0f)
			return std::unexpected(MakeError(AudioSessionErrorCode::InvalidMixSettings,
				"User master gain must be finite and between zero and one"));
		m_Impl->UserOverrides.Master = Gain;
		m_Impl->SettingsDirty = true;
		return m_Impl->ApplyMix();
	}

	std::expected<void, AudioSessionError> AudioSession::SetUserBusGain(AudioBus Bus, float Gain)
	{
		if (!m_Impl || !m_Impl->IsOwnerThread())
			return std::unexpected(MakeError(AudioSessionErrorCode::WrongThread,
				"Audio mix changes must run on the owning session thread"));
		if (!IsValidBus(Bus) || !std::isfinite(Gain) || Gain < 0.0f || Gain > 1.0f)
			return std::unexpected(MakeError(AudioSessionErrorCode::InvalidMixSettings,
				"User bus gain requires a supported bus and a finite value between zero and one"));
		switch (Bus)
		{
			case AudioBus::Music: m_Impl->UserOverrides.Music = Gain; break;
			case AudioBus::Effects: m_Impl->UserOverrides.Effects = Gain; break;
			case AudioBus::UI: m_Impl->UserOverrides.UI = Gain; break;
		}
		m_Impl->SettingsDirty = true;
		return m_Impl->ApplyMix();
	}

	std::expected<void, AudioSessionError> AudioSession::SetMasterMuted(bool Muted)
	{
		if (!m_Impl || !m_Impl->IsOwnerThread())
			return std::unexpected(MakeError(AudioSessionErrorCode::WrongThread,
				"Audio mix changes must run on the owning session thread"));
		m_Impl->UserOverrides.MasterMuted = Muted;
		m_Impl->SettingsDirty = true;
		return m_Impl->ApplyMix();
	}

	std::expected<void, AudioSessionError> AudioSession::SetBusMuted(AudioBus Bus, bool Muted)
	{
		if (!m_Impl || !m_Impl->IsOwnerThread())
			return std::unexpected(MakeError(AudioSessionErrorCode::WrongThread,
				"Audio mix changes must run on the owning session thread"));
		if (!IsValidBus(Bus))
			return std::unexpected(MakeError(AudioSessionErrorCode::InvalidMixSettings, "Cannot mute an unsupported audio bus"));
		switch (Bus)
		{
			case AudioBus::Music: m_Impl->UserOverrides.MusicMuted = Muted; break;
			case AudioBus::Effects: m_Impl->UserOverrides.EffectsMuted = Muted; break;
			case AudioBus::UI: m_Impl->UserOverrides.UIMuted = Muted; break;
		}
		m_Impl->SettingsDirty = true;
		return m_Impl->ApplyMix();
	}

	std::expected<void, AudioSessionError> AudioSession::ResetUserMixOverrides()
	{
		if (!m_Impl || !m_Impl->IsOwnerThread())
			return std::unexpected(MakeError(AudioSessionErrorCode::WrongThread,
				"Audio mix changes must run on the owning session thread"));
		m_Impl->UserOverrides = {};
		m_Impl->SettingsDirty = true;
		return m_Impl->ApplyMix();
	}

	std::expected<void, AudioSessionError> AudioSession::SaveUserSettings()
	{
		if (!m_Impl || std::this_thread::get_id() != m_Impl->OwnerThread)
			return std::unexpected(MakeError(AudioSessionErrorCode::WrongThread,
				"Audio settings must be saved on the owning session thread"));
		if (m_Impl->SettingsPath.empty())
			return std::unexpected(MakeError(AudioSessionErrorCode::SettingsUnavailable,
				"User audio settings path is unavailable: " + m_Impl->SettingsDiagnostic));

		try
		{
			Json Overrides = Json::object();
			if (m_Impl->UserOverrides.Master) Overrides["master"] = *m_Impl->UserOverrides.Master;
			if (m_Impl->UserOverrides.Music) Overrides["music"] = *m_Impl->UserOverrides.Music;
			if (m_Impl->UserOverrides.Effects) Overrides["effects"] = *m_Impl->UserOverrides.Effects;
			if (m_Impl->UserOverrides.UI) Overrides["ui"] = *m_Impl->UserOverrides.UI;
			const Json Document = Json::object({
				{ "version", 1 },
				{ "projectId", m_Impl->ProjectID.ToString() },
				{ "overrides", std::move(Overrides) },
				{ "mutes", Json::object({
					{ "master", m_Impl->UserOverrides.MasterMuted },
					{ "music", m_Impl->UserOverrides.MusicMuted },
					{ "effects", m_Impl->UserOverrides.EffectsMuted },
					{ "ui", m_Impl->UserOverrides.UIMuted }
				}) }
			});
			const std::string Contents = Document.dump(2) + "\n";
			if (Contents.size() > MaximumAudioSettingsBytes)
				return std::unexpected(MakeError(AudioSessionErrorCode::SettingsWriteFailed,
					"Serialized audio settings exceed the 64 KiB limit"));
			std::error_code Error;
			std::filesystem::create_directories(m_Impl->SettingsPath.parent_path(), Error);
			if (Error)
				return std::unexpected(MakeError(AudioSessionErrorCode::SettingsWriteFailed,
					"Could not create user audio settings directory: " + Error.message()));
			const auto TemporaryID = UUID::Generate();
			if (!TemporaryID)
				return std::unexpected(MakeError(AudioSessionErrorCode::SettingsWriteFailed, TemporaryID.error().Message));
			std::filesystem::path TemporaryPath = m_Impl->SettingsPath.parent_path() /
				(".PulseForge.AudioSettings." + TemporaryID->ToString() + ".tmp");
			auto Written = WriteExclusiveSettingsFile(TemporaryPath, Contents);
			if (!Written)
				return std::unexpected(MakeError(AudioSessionErrorCode::SettingsWriteFailed, Written.error()));
			ScopedOwnedTemporaryPath Cleanup(TemporaryPath);
			Cleanup.TakeOwnership();
			if (auto Replaced = Platform::ReplaceFileAtomically(TemporaryPath, m_Impl->SettingsPath); !Replaced)
				return std::unexpected(MakeError(AudioSessionErrorCode::SettingsReplaceFailed, Replaced.error().Message));
			Cleanup.Release();
			m_Impl->SettingsDirty = false;
			m_Impl->SettingsDiagnostic.clear();
			return {};
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(MakeError(AudioSessionErrorCode::SettingsWriteFailed,
				std::string("Could not serialize user audio settings: ") + Exception.what()));
		}
	}
}
