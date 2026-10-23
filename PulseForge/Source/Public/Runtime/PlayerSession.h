#pragma once

#include "Assets/AssetID.h"
#include "Core/Core.h"
#include "Core/Timestep.h"
#include "Runtime/SceneRuntime.h"

#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace PulseForge
{
	class Project;
	class Scene;

	enum class PlayerSessionErrorCode : uint8_t
	{
		AlreadyStarted,
		NotStarted,
		MissingStartupScene,
		InvalidSceneAsset,
		InvalidDeltaTime,
		InvalidTimeScale,
		ProjectOpenFailed,
		SceneLoadFailed,
		RuntimeStartFailed,
		RuntimeOperationFailed
	};

	struct PlayerSessionError
	{
		PlayerSessionErrorCode Code;
		std::string Message;
	};

	// Owns one project and a stable active scene/runtime pair. Non-owning services must outlive the session.
	// Stop ends runtime state without saving mutations made during play.
	class PULSEFORGE_API PlayerSession final
	{
	public:
		explicit PlayerSession(
			std::filesystem::path ProjectFile, SceneRuntimeDesc RuntimeDescription = {}, SceneRuntimeServices RuntimeServices = {});
		~PlayerSession();
		PlayerSession(const PlayerSession&) = delete;
		PlayerSession& operator=(const PlayerSession&) = delete;

		[[nodiscard]] std::expected<void, PlayerSessionError> Start(std::optional<AssetID> StartupSceneOverride = std::nullopt);
		[[nodiscard]] std::expected<void, PlayerSessionError> RequestSceneTransition(const AssetID& SceneAsset);
		// Commits a prepared transition at the beginning of this frame, then advances the active scene unless paused.
		[[nodiscard]] std::expected<SceneRuntimeFrameResult, PlayerSessionError> Advance(Timestep FrameDelta);
		[[nodiscard]] std::expected<void, PlayerSessionError> Pause();
		[[nodiscard]] std::expected<void, PlayerSessionError> Resume();
		// Positive finite scales affect script/physics frame time; audio playback follows real time. Use Pause for full pause.
		[[nodiscard]] std::expected<void, PlayerSessionError> SetTimeScale(double Scale);
		[[nodiscard]] double GetTimeScale() const noexcept { return m_TimeScale; }
		[[nodiscard]] bool IsPaused() const noexcept { return m_Paused; }
		[[nodiscard]] bool IsRunning() const noexcept { return m_Runtime != nullptr; }
		void Stop() noexcept;

		[[nodiscard]] const Project* GetProject() const noexcept { return m_Project.get(); }
		[[nodiscard]] Scene* GetActiveScene() noexcept { return m_Scene.get(); }
		[[nodiscard]] const Scene* GetActiveScene() const noexcept { return m_Scene.get(); }
		[[nodiscard]] const std::optional<AssetID>& GetActiveSceneAsset() const noexcept { return m_SceneAsset; }

	private:
		struct PendingScene;

		std::filesystem::path m_ProjectFile;
		SceneRuntimeDesc m_RuntimeDescription;
		SceneRuntimeServices m_RuntimeServices;
		// Declaration order ensures runtime teardown precedes scene and project teardown.
		std::unique_ptr<Project> m_Project;
		std::unique_ptr<Scene> m_Scene;
		std::unique_ptr<SceneRuntime> m_Runtime;
		std::unique_ptr<PendingScene> m_PendingScene;
		std::optional<AssetID> m_SceneAsset;
		bool m_Paused = false;
		double m_TimeScale = 1.0;
	};
} // namespace PulseForge
