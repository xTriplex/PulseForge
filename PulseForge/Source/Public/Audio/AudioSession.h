#pragma once

#include "Audio/AudioEngine.h"
#include "Audio/AudioMixSettings.h"
#include "Core/Core.h"

#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace PulseForge
{
	class AudioAssetCache;
	class CookedContentProvider;
	class DiagnosticRecorder;
	class UUID;
#if !defined(PF_COOKED_RUNTIME_ONLY)
	class Project;
#endif

	enum class AudioDeviceState : uint8_t
	{
		DefaultActive,
		ExplicitNull,
		Unavailable,
		Recovering,
		RecoveryFailed,
		OfflineTest
	};

	enum class AudioSessionErrorCode : uint8_t
	{
		InvalidDescription,
		EngineInitializationFailed,
		AssetProviderUnavailable,
		InvalidMixSettings,
		SettingsUnavailable,
		SettingsReadFailed,
		SettingsWriteFailed,
		SettingsReplaceFailed,
		WrongThread,
		DeviceRecoveryFailed
	};

	struct AudioSessionError final
	{
		AudioSessionErrorCode Code;
		std::string Message;
	};

	struct AudioSessionDesc final
	{
		AudioEngineDesc Engine;
		// Empty selects Platform::GetUserDataDirectory(); tests may inject a separate absolute root.
		std::filesystem::path UserDataDirectory;
		DiagnosticRecorder* Diagnostics = nullptr;
		// Optional engine factory for host integration and deterministic output-failure tests; empty uses AudioEngine::Create.
		std::function<std::expected<AudioEngine, AudioError>(AudioOutputBackend)> EngineFactory;
	};

	// Session-lifetime device, project/cooked clip cache, user mix, and settings persistence.
	// Scene voices remain owned by AudioSceneRuntime and borrow the stable engine/cache objects.
	// Mutating operations must run on the thread which created the session.
	class PULSEFORGE_API AudioSession final
	{
	public:
#if !defined(PF_COOKED_RUNTIME_ONLY)
		[[nodiscard]] static std::expected<std::unique_ptr<AudioSession>, AudioSessionError> CreateForProject(
			const Project& SourceProject, AudioSessionDesc Description = {});
#endif
		[[nodiscard]] static std::expected<std::unique_ptr<AudioSession>, AudioSessionError> CreateForCooked(
			std::shared_ptr<const CookedContentProvider> CookedAssets,
			AudioMixGains AuthoredDefaults = {},
			AudioSessionDesc Description = {});
		~AudioSession();
		AudioSession(const AudioSession&) = delete;
		AudioSession& operator=(const AudioSession&) = delete;

		[[nodiscard]] AudioEngine& GetEngine() noexcept;
		[[nodiscard]] AudioAssetCache& GetAssets() noexcept;
		[[nodiscard]] uint64_t GetGeneration() const noexcept;
		[[nodiscard]] const UUID& GetProjectID() const noexcept;
		[[nodiscard]] AudioDeviceState GetDeviceState() const noexcept;
		[[nodiscard]] std::string_view GetDeviceDiagnostic() const noexcept;
		[[nodiscard]] std::string_view GetSettingsDiagnostic() const noexcept;
		[[nodiscard]] const std::filesystem::path& GetSettingsPath() const noexcept;
		[[nodiscard]] AudioMixState GetMixState() const noexcept;
		[[nodiscard]] bool HasUnsavedUserSettings() const noexcept;

		// Call on the session owner thread. Device callbacks only publish values; this method consumes them.
		void PollDeviceState() noexcept;
		// Explicit retry; a successful device replacement drops old scene voices rather than replaying them silently.
		[[nodiscard]] std::expected<void, AudioSessionError> RetryDefaultOutput();
		[[nodiscard]] std::expected<void, AudioSessionError> SetUserMasterGain(float Gain);
		[[nodiscard]] std::expected<void, AudioSessionError> SetUserBusGain(AudioBus Bus, float Gain);
		[[nodiscard]] std::expected<void, AudioSessionError> SetMasterMuted(bool Muted);
		[[nodiscard]] std::expected<void, AudioSessionError> SetBusMuted(AudioBus Bus, bool Muted);
		[[nodiscard]] std::expected<void, AudioSessionError> ResetUserMixOverrides();
		[[nodiscard]] std::expected<void, AudioSessionError> SaveUserSettings();

	private:
		struct Impl;
		explicit AudioSession(std::unique_ptr<Impl> Implementation);
		std::unique_ptr<Impl> m_Impl;
	};
}
