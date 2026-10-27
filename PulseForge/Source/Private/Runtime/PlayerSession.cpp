#include "Runtime/PlayerSession.h"
#include "Runtime/CookedContentProvider.h"

#if !defined(PF_COOKED_RUNTIME_ONLY)
#include "Assets/Project.h"
#include "Assets/SceneAssetService.h"
#endif
#include "Scene/Scene.h"

#include <cmath>
#include <utility>

namespace PulseForge
{
	struct PlayerSession::PendingScene final
	{
		AssetID Asset;
		std::unique_ptr<Scene> Value;
	};

	namespace
	{
		PlayerSessionError MakeError(PlayerSessionErrorCode Code, std::string Message)
		{
			return { Code, std::move(Message) };
		}
	} // namespace

#if !defined(PF_COOKED_RUNTIME_ONLY)
	PlayerSession::PlayerSession(
		std::filesystem::path ProjectFile, SceneRuntimeDesc RuntimeDescription, SceneRuntimeServices RuntimeServices)
		: m_ProjectFile(std::move(ProjectFile)), m_RuntimeDescription(std::move(RuntimeDescription)), m_RuntimeServices(RuntimeServices)
	{
	}
#endif

	PlayerSession::PlayerSession(
		std::shared_ptr<const CookedContentProvider> CookedAssets,
		SceneRuntimeDesc RuntimeDescription,
		SceneRuntimeServices RuntimeServices)
		: m_CookedAssets(std::move(CookedAssets)), m_RuntimeDescription(std::move(RuntimeDescription)), m_RuntimeServices(RuntimeServices)
	{
	}

	PlayerSession::~PlayerSession()
	{
		Stop();
	}

	std::expected<void, PlayerSessionError> PlayerSession::Start(std::optional<AssetID> StartupSceneOverride)
	{
		if (IsRunning()) return std::unexpected(MakeError(PlayerSessionErrorCode::AlreadyStarted, "Player session is already running"));

		if (m_CookedAssets)
		{
			const AssetID StartupScene = StartupSceneOverride.value_or(m_CookedAssets->GetStartupScene());
			if (StartupScene.IsNil())
				return std::unexpected(MakeError(PlayerSessionErrorCode::InvalidSceneAsset, "Cooked startup scene UUID must not be nil"));
			auto CandidateScene = m_CookedAssets->LoadScene(StartupScene);
			if (!CandidateScene)
				return std::unexpected(MakeError(PlayerSessionErrorCode::SceneLoadFailed,
					"Could not load cooked startup scene: " + CandidateScene.error().Message));
			auto CandidateRuntime = std::make_unique<SceneRuntime>(m_CookedAssets, m_RuntimeDescription, m_RuntimeServices);
			if (auto Started = CandidateRuntime->Start(**CandidateScene); !Started)
				return std::unexpected(MakeError(PlayerSessionErrorCode::RuntimeStartFailed,
					"Could not start cooked player runtime: " + Started.error().Message));
			m_Scene = std::move(*CandidateScene);
			m_Runtime = std::move(CandidateRuntime);
			m_SceneAsset = StartupScene;
			m_PendingScene.reset();
			m_Paused = false;
			return {};
		}

#if !defined(PF_COOKED_RUNTIME_ONLY)
		auto OpenedProject = Project::Open(m_ProjectFile, m_RuntimeServices.Diagnostics);
		if (!OpenedProject)
			return std::unexpected(
				MakeError(PlayerSessionErrorCode::ProjectOpenFailed, "Could not open player project: " + OpenedProject.error().Message));

		auto CandidateProject = std::make_unique<Project>(std::move(*OpenedProject));
		const std::optional<AssetID> StartupScene =
			StartupSceneOverride ? StartupSceneOverride : CandidateProject->GetDescription().StartScene;
		if (!StartupScene)
			return std::unexpected(MakeError(PlayerSessionErrorCode::MissingStartupScene,
				"Player project does not specify a startup scene and no override was supplied"));
		if (StartupScene->IsNil())
			return std::unexpected(MakeError(PlayerSessionErrorCode::InvalidSceneAsset, "Player startup scene UUID must not be nil"));

		auto CandidateScene = std::make_unique<Scene>();
		if (auto Loaded = SceneAssetService::Load(*StartupScene, CandidateProject->GetRootPath(), CandidateProject->GetAssetRegistry(),
				*CandidateScene, m_RuntimeServices.Diagnostics);
			!Loaded)
			return std::unexpected(
				MakeError(PlayerSessionErrorCode::SceneLoadFailed, "Could not load player startup scene: " + Loaded.error().Message));

		auto CandidateRuntime = std::make_unique<SceneRuntime>(*CandidateProject, m_RuntimeDescription, m_RuntimeServices);
		if (auto Started = CandidateRuntime->Start(*CandidateScene); !Started)
			return std::unexpected(
				MakeError(PlayerSessionErrorCode::RuntimeStartFailed, "Could not start player scene runtime: " + Started.error().Message));

		m_Project = std::move(CandidateProject);
		m_Scene = std::move(CandidateScene);
		m_Runtime = std::move(CandidateRuntime);
		m_SceneAsset = *StartupScene;
		m_PendingScene.reset();
		m_Paused = false;
		return {};
#else
		return std::unexpected(MakeError(PlayerSessionErrorCode::ProjectOpenFailed,
			"Cooked player session requires its cooked content provider"));
#endif
	}

	std::expected<void, PlayerSessionError> PlayerSession::RequestSceneTransition(const AssetID& SceneAsset)
	{
		if (!IsRunning()) return std::unexpected(MakeError(PlayerSessionErrorCode::NotStarted, "Player session is not running"));
		if (SceneAsset.IsNil())
			return std::unexpected(MakeError(PlayerSessionErrorCode::InvalidSceneAsset, "Transition scene UUID must not be nil"));

		auto CandidateScene = std::make_unique<Scene>();
		auto Candidate = std::make_unique<PendingScene>();
		if (m_CookedAssets)
		{
			auto Loaded = m_CookedAssets->LoadScene(SceneAsset);
			if (!Loaded)
				return std::unexpected(MakeError(PlayerSessionErrorCode::SceneLoadFailed,
					"Could not prepare cooked scene transition: " + Loaded.error().Message));
			Candidate->Value = std::move(*Loaded);
		}
#if !defined(PF_COOKED_RUNTIME_ONLY)
		else if (auto Loaded = SceneAssetService::Load(
				SceneAsset, m_Project->GetRootPath(), m_Project->GetAssetRegistry(), *CandidateScene, m_RuntimeServices.Diagnostics);
			!Loaded)
			return std::unexpected(
				MakeError(PlayerSessionErrorCode::SceneLoadFailed, "Could not prepare player scene transition: " + Loaded.error().Message));

		Candidate->Asset = SceneAsset;
		if (!m_CookedAssets) Candidate->Value = std::move(CandidateScene);
#else
		else
			return std::unexpected(MakeError(PlayerSessionErrorCode::SceneLoadFailed,
				"Cooked scene transitions require a cooked content provider"));
#endif
		m_PendingScene = std::move(Candidate);
		return {};
	}

	std::expected<SceneRuntimeFrameResult, PlayerSessionError> PlayerSession::Advance(Timestep FrameDelta)
	{
		if (!IsRunning()) return std::unexpected(MakeError(PlayerSessionErrorCode::NotStarted, "Player session is not running"));
		const double DeltaSeconds = FrameDelta.GetSeconds();
		if (!std::isfinite(DeltaSeconds) || DeltaSeconds < 0.0)
			return std::unexpected(
				MakeError(PlayerSessionErrorCode::InvalidDeltaTime, "Player frame delta must be finite and non-negative"));
		const double ScaledDeltaSeconds = DeltaSeconds * m_TimeScale;
		if (!std::isfinite(ScaledDeltaSeconds))
			return std::unexpected(MakeError(PlayerSessionErrorCode::InvalidDeltaTime, "Scaled player frame delta is not finite"));

		if (m_PendingScene)
		{
			// Move the candidate scene into a local owner declared before its runtime. On every
			// return and exception path, the runtime is destroyed first and can still use the scene
			// from script teardown callbacks.
			auto CandidateScene = std::move(m_PendingScene->Value);
			const AssetID CandidateAsset = m_PendingScene->Asset;
			m_PendingScene.reset();
#if defined(PF_COOKED_RUNTIME_ONLY)
			auto CandidateRuntime = std::make_unique<SceneRuntime>(m_CookedAssets, m_RuntimeDescription, m_RuntimeServices);
#else
			auto CandidateRuntime = m_CookedAssets
				? std::make_unique<SceneRuntime>(m_CookedAssets, m_RuntimeDescription, m_RuntimeServices)
				: std::make_unique<SceneRuntime>(*m_Project, m_RuntimeDescription, m_RuntimeServices);
#endif
			if (auto Started = CandidateRuntime->Start(*CandidateScene); !Started)
			{
				return std::unexpected(MakeError(
					PlayerSessionErrorCode::RuntimeStartFailed, "Could not start requested player scene: " + Started.error().Message));
			}
			if (auto Paused = CandidateRuntime->SetPaused(m_Paused); !Paused)
			{
				return std::unexpected(MakeError(PlayerSessionErrorCode::RuntimeOperationFailed,
					"Could not apply player pause state to requested scene: " + Paused.error().Message));
			}

			// Keep the old scene alive until its runtime has released every non-owning scene reference.
			auto RetiredScene = std::move(m_Scene);
			auto RetiredRuntime = std::move(m_Runtime);
			m_Scene = std::move(CandidateScene);
			m_SceneAsset = CandidateAsset;
			m_Runtime = std::move(CandidateRuntime);
			RetiredRuntime.reset();
			RetiredScene.reset();
		}

		if (m_Paused) return SceneRuntimeFrameResult{};

		auto Result = m_Runtime->Advance(*m_Scene, Timestep(ScaledDeltaSeconds));
		if (!Result)
			return std::unexpected(MakeError(
				PlayerSessionErrorCode::RuntimeOperationFailed, "Could not advance player scene runtime: " + Result.error().Message));
		return *Result;
	}

	std::expected<void, PlayerSessionError> PlayerSession::SetTimeScale(double Scale)
	{
		if (!std::isfinite(Scale) || Scale <= 0.0)
			return std::unexpected(MakeError(PlayerSessionErrorCode::InvalidTimeScale,
				"Player time scale must be finite and greater than zero; use Pause to stop simulation and audio."));
		m_TimeScale = Scale;
		return {};
	}

	std::expected<void, PlayerSessionError> PlayerSession::Pause()
	{
		if (!IsRunning()) return std::unexpected(MakeError(PlayerSessionErrorCode::NotStarted, "Player session is not running"));
		if (m_Paused) return {};

		if (auto Result = m_Runtime->SetPaused(true); !Result)
			return std::unexpected(MakeError(
				PlayerSessionErrorCode::RuntimeOperationFailed, "Could not pause player scene runtime: " + Result.error().Message));
		m_Paused = true;
		return {};
	}

	std::expected<void, PlayerSessionError> PlayerSession::Resume()
	{
		if (!IsRunning()) return std::unexpected(MakeError(PlayerSessionErrorCode::NotStarted, "Player session is not running"));
		if (!m_Paused) return {};

		if (auto Result = m_Runtime->SetPaused(false); !Result)
			return std::unexpected(MakeError(
				PlayerSessionErrorCode::RuntimeOperationFailed, "Could not resume player scene runtime: " + Result.error().Message));
		m_Paused = false;
		return {};
	}

	void PlayerSession::Stop() noexcept
	{
		m_PendingScene.reset();
		m_Runtime.reset();
		m_Scene.reset();
		m_SceneAsset.reset();
#if !defined(PF_COOKED_RUNTIME_ONLY)
		m_Project.reset();
#endif
		m_Paused = false;
		m_TimeScale = 1.0;
	}
} // namespace PulseForge
