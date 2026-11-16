#include "Runtime/PlayerSession.h"
#include "Runtime/CookedContentProvider.h"
#include "Audio/AudioSession.h"
#include "Assets/AssetTypes.h"
#include "Core/BoundedInput.h"
#include "Scripting/BlueprintAsset.h"

#if !defined(PF_COOKED_RUNTIME_ONLY)
#include "Assets/Project.h"
#include "Assets/SceneAssetService.h"
#endif
#include "Scene/Scene.h"

#include <cmath>
#include <algorithm>
#include <fstream>
#include <unordered_map>
#include <utility>

namespace PulseForge
{
	struct PlayerSession::PendingScene final
	{
		AssetID Asset;
		std::unique_ptr<Scene> Value;
		std::optional<SceneRuntimeBoundaryRequest> OriginRequest;
	};

	namespace
	{
		PlayerSessionError MakeError(PlayerSessionErrorCode Code, std::string Message)
		{
			return { Code, std::move(Message) };
		}

		void AdvanceGeneration(uint64_t& Generation) noexcept
		{
			++Generation;
			if (Generation == 0) ++Generation;
		}

		void BindSceneBlueprintServices(
			SceneRuntimeServices& Services,
			const std::shared_ptr<const CookedContentProvider>& CookedAssets,
			const Project* SourceProject)
		{
			using BlueprintAssetCache = std::unordered_map<AssetID, std::shared_ptr<const VerifiedBlueprintAsset>, UUIDHash>;
			auto Cache = std::make_shared<BlueprintAssetCache>();
			const std::shared_ptr<const CookedContentProvider> Provider = CookedAssets;
			Services.ResolveBlueprintProgram = [Cache, Provider, SourceProject](const Entity& Target, BlueprintEventKind Event)
				-> std::optional<BlueprintProgramBinding>
			{
				const auto Component = Target.GetBlueprint();
				if (!Component || !Component->has_value() || !(**Component).Enabled) return std::nullopt;
				const AssetID Asset = (**Component).BlueprintAsset;
				auto Found = Cache->find(Asset);
				if (Found == Cache->end())
				{
					std::expected<VerifiedBlueprintAsset, std::string> Loaded = std::unexpected("Blueprint asset is unavailable");
					if (Provider)
					{
						auto Cooked = Provider->LoadBlueprintProgram(Asset);
						if (Cooked) Loaded = std::move(*Cooked);
						else Loaded = std::unexpected(Cooked.error().Message);
					}
#if !defined(PF_COOKED_RUNTIME_ONLY)
					else if (SourceProject)
					{
						const auto Record = SourceProject->GetAssetRegistry().Find(Asset);
						if (Record && HasAssetTypeExtension(Record->ProjectRelativePath, AssetFileType::Blueprint))
						{
							std::ifstream Input(SourceProject->GetRootPath() / Record->ProjectRelativePath, std::ios::binary);
							auto Bytes = Input ? Detail::ReadBoundedInput(Input, BlueprintMaximumAssetBytes)
								: std::expected<std::string, Detail::BoundedInputError>(std::unexpected(Detail::BoundedInputError{
									Detail::BoundedInputErrorCode::ReadFailed }));
							if (Bytes)
							{
								std::vector<std::byte> Buffer(Bytes->size());
								std::transform(Bytes->begin(), Bytes->end(), Buffer.begin(),
									[](char Value) { return static_cast<std::byte>(static_cast<unsigned char>(Value)); });
								auto Decoded = DeserializeBlueprintAsset(Buffer);
								if (Decoded) Loaded = std::move(*Decoded);
								else Loaded = std::unexpected(Decoded.error().Message);
							}
						}
					}
#endif
					if (!Loaded) return std::nullopt;
					Found = Cache->emplace(Asset, std::make_shared<const VerifiedBlueprintAsset>(std::move(*Loaded))).first;
				}
				for (const BlueprintVerifiedEventProgram& Candidate : Found->second->GetEvents())
				{
					if (Candidate.Event != Event) continue;
					return BlueprintProgramBinding{ Asset, 1,
						std::make_shared<const VerifiedBlueprintProgram>(Candidate.Program) };
				}
				return std::nullopt;
			};
			if (!Services.GetBlueprintProgramGeneration) Services.GetBlueprintProgramGeneration = [] { return uint64_t{ 1 }; };
			if (!Services.ReadBlueprintScalarProperty)
			{
				Services.ReadBlueprintScalarProperty = [](const Entity& Target, std::string_view PropertyId) -> std::optional<float>
				{
					const auto Existing = Target.GetBlueprint();
					return Existing && Existing->has_value() ? (**Existing).FindFloatProperty(PropertyId) : std::nullopt;
				};
			}
			if (!Services.ApplyBlueprintPropertyWrites)
			{
				Services.ApplyBlueprintPropertyWrites = [](const Entity& Target, std::span<const BlueprintScalarPropertyWrite> Writes)
				{
					auto Existing = Target.GetBlueprint();
					if (!Existing || !Existing->has_value()) return false;
					BlueprintComponent Candidate = **Existing;
					for (const BlueprintScalarPropertyWrite& Write : Writes)
						if (!Candidate.SetFloatProperty(Write.PropertyId, Write.Value)) return false;
					return Target.SetBlueprint(Candidate).has_value();
				};
			}
		}
	} // namespace

#if !defined(PF_COOKED_RUNTIME_ONLY)
	PlayerSession::PlayerSession(
		std::filesystem::path ProjectFile, SceneRuntimeDesc RuntimeDescription, SceneRuntimeServices RuntimeServices)
		: m_ProjectFile(std::move(ProjectFile)), m_RuntimeDescription(std::move(RuntimeDescription)), m_RuntimeServices(RuntimeServices)
	{
		if (!m_RuntimeServices.QueueSceneTransition)
			m_RuntimeServices.QueueSceneTransition = [this](const AssetID& Asset)
			{
				if (!IsRunning() || m_PendingScene || m_RequestedSceneTransition) return false;
				m_RequestedSceneTransition = Asset;
				return true;
			};
		if (!m_RuntimeServices.QueueStop)
			m_RuntimeServices.QueueStop = [this]()
			{
				if (!IsRunning()) return false;
				m_StopRequested = true;
				return true;
			};
	}
#endif

	PlayerSession::PlayerSession(
		std::shared_ptr<const CookedContentProvider> CookedAssets,
		SceneRuntimeDesc RuntimeDescription,
		SceneRuntimeServices RuntimeServices)
		: m_CookedAssets(std::move(CookedAssets)), m_RuntimeDescription(std::move(RuntimeDescription)), m_RuntimeServices(RuntimeServices)
	{
		if (!m_RuntimeServices.QueueSceneTransition)
			m_RuntimeServices.QueueSceneTransition = [this](const AssetID& Asset)
			{
				if (!IsRunning() || m_PendingScene || m_RequestedSceneTransition) return false;
				m_RequestedSceneTransition = Asset;
				return true;
			};
		if (!m_RuntimeServices.QueueStop)
			m_RuntimeServices.QueueStop = [this]()
			{
				if (!IsRunning()) return false;
				m_StopRequested = true;
				return true;
			};
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
			auto CandidateAudioSession = AudioSession::CreateForCooked(
				m_CookedAssets, m_CookedAssets->GetAudioMixDefaults(), AudioSessionDesc{ .Engine = m_RuntimeDescription.Audio,
					.Diagnostics = m_RuntimeServices.Diagnostics });
			if (!CandidateAudioSession)
				return std::unexpected(MakeError(PlayerSessionErrorCode::RuntimeStartFailed,
					"Could not create cooked player audio session: " + CandidateAudioSession.error().Message));
			auto CandidateServices = m_RuntimeServices;
			CandidateServices.AudioSessionState = CandidateAudioSession->get();
			BindSceneBlueprintServices(CandidateServices, m_CookedAssets, nullptr);
			auto CandidateRuntime = std::make_unique<SceneRuntime>(m_CookedAssets, m_RuntimeDescription, CandidateServices);
			if (auto Started = CandidateRuntime->Start(**CandidateScene); !Started)
				return std::unexpected(MakeError(PlayerSessionErrorCode::RuntimeStartFailed,
					"Could not start cooked player runtime: " + Started.error().Message));
			m_AudioSession = std::move(*CandidateAudioSession);
			m_Scene = std::move(*CandidateScene);
			m_Runtime = std::move(CandidateRuntime);
			m_SceneAsset = StartupScene;
			m_PendingScene.reset();
			m_Paused = false;
			AdvanceGeneration(m_RuntimeUISessionGeneration);
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

		auto CandidateAudioSession = AudioSession::CreateForProject(*CandidateProject,
			AudioSessionDesc{ .Engine = m_RuntimeDescription.Audio, .Diagnostics = m_RuntimeServices.Diagnostics });
		if (!CandidateAudioSession)
			return std::unexpected(MakeError(PlayerSessionErrorCode::RuntimeStartFailed,
				"Could not create player audio session: " + CandidateAudioSession.error().Message));
		auto CandidateServices = m_RuntimeServices;
		CandidateServices.AudioSessionState = CandidateAudioSession->get();
		BindSceneBlueprintServices(CandidateServices, {}, CandidateProject.get());
		auto CandidateRuntime = std::make_unique<SceneRuntime>(*CandidateProject, m_RuntimeDescription, CandidateServices);
		if (auto Started = CandidateRuntime->Start(*CandidateScene); !Started)
			return std::unexpected(
				MakeError(PlayerSessionErrorCode::RuntimeStartFailed, "Could not start player scene runtime: " + Started.error().Message));

		m_Project = std::move(CandidateProject);
		m_AudioSession = std::move(*CandidateAudioSession);
		m_Scene = std::move(CandidateScene);
		m_Runtime = std::move(CandidateRuntime);
		m_SceneAsset = *StartupScene;
		m_PendingScene.reset();
		m_Paused = false;
		AdvanceGeneration(m_RuntimeUISessionGeneration);
		return {};
#else
		return std::unexpected(MakeError(PlayerSessionErrorCode::ProjectOpenFailed,
			"Cooked player session requires its cooked content provider"));
#endif
	}

	std::expected<void, PlayerSessionError> PlayerSession::RequestSceneTransition(const AssetID& SceneAsset)
	{
		if (!IsRunning()) return std::unexpected(MakeError(PlayerSessionErrorCode::NotStarted, "Player session is not running"));
		if (m_PendingScene)
			return std::unexpected(MakeError(PlayerSessionErrorCode::RuntimeOperationFailed,
				"A scene transition is already pending"));
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

		if (!m_CookedAssets) Candidate->Value = std::move(CandidateScene);
#else
		else
			return std::unexpected(MakeError(PlayerSessionErrorCode::SceneLoadFailed,
				"Cooked scene transitions require a cooked content provider"));
#endif
		Candidate->Asset = SceneAsset;
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
			const auto OriginRequest = m_PendingScene->OriginRequest;
			m_PendingScene.reset();
#if defined(PF_COOKED_RUNTIME_ONLY)
			auto CandidateServices = m_RuntimeServices;
			CandidateServices.AudioSessionState = m_AudioSession.get();
			BindSceneBlueprintServices(CandidateServices, m_CookedAssets, nullptr);
			auto CandidateRuntime = std::make_unique<SceneRuntime>(m_CookedAssets, m_RuntimeDescription, CandidateServices);
#else
			auto CandidateServices = m_RuntimeServices;
			CandidateServices.AudioSessionState = m_AudioSession.get();
			BindSceneBlueprintServices(CandidateServices, m_CookedAssets, m_Project.get());
			auto CandidateRuntime = m_CookedAssets
				? std::make_unique<SceneRuntime>(m_CookedAssets, m_RuntimeDescription, CandidateServices)
				: std::make_unique<SceneRuntime>(*m_Project, m_RuntimeDescription, CandidateServices);
#endif
			if (auto Started = CandidateRuntime->Start(*CandidateScene, true); !Started)
			{
				if (OriginRequest && m_Runtime)
					(void)m_Runtime->ResolveBoundaryRequest(*OriginRequest, SceneRuntimeBoundaryResolution::Rejected);
				return std::unexpected(MakeError(
					PlayerSessionErrorCode::RuntimeStartFailed, "Could not start requested player scene: " + Started.error().Message));
			}
			if (auto Paused = CandidateRuntime->SetPaused(m_Paused); !Paused)
			{
				if (OriginRequest && m_Runtime)
					(void)m_Runtime->ResolveBoundaryRequest(*OriginRequest, SceneRuntimeBoundaryResolution::Rejected);
				return std::unexpected(MakeError(PlayerSessionErrorCode::RuntimeOperationFailed,
					"Could not apply player pause state to requested scene: " + Paused.error().Message));
			}
			if (auto Suspended = m_Runtime->SuspendAudioForTransition(); !Suspended)
			{
				if (OriginRequest && m_Runtime)
					(void)m_Runtime->ResolveBoundaryRequest(*OriginRequest, SceneRuntimeBoundaryResolution::Rejected);
				return std::unexpected(MakeError(PlayerSessionErrorCode::RuntimeOperationFailed,
					"Could not suspend outgoing audio for scene transition: " + Suspended.error().Message));
			}
			if (auto Activated = CandidateRuntime->ActivateAudio(); !Activated)
			{
				auto Resumed = m_Runtime->ResumeAudioAfterTransition();
				if (OriginRequest && m_Runtime)
					(void)m_Runtime->ResolveBoundaryRequest(*OriginRequest, SceneRuntimeBoundaryResolution::Rejected);
				const std::string ResumeDetail = Resumed ? std::string{} :
					"; outgoing audio resume also failed: " + Resumed.error().Message;
				return std::unexpected(MakeError(PlayerSessionErrorCode::RuntimeOperationFailed,
					"Could not activate requested scene audio: " + Activated.error().Message + ResumeDetail));
			}
			if (OriginRequest && m_Runtime)
				(void)m_Runtime->ResolveBoundaryRequest(*OriginRequest, SceneRuntimeBoundaryResolution::Committed);

			// Keep the old scene alive until its runtime has released every non-owning scene reference.
			auto RetiredScene = std::move(m_Scene);
			auto RetiredRuntime = std::move(m_Runtime);
			m_Scene = std::move(CandidateScene);
			m_SceneAsset = CandidateAsset;
			m_Runtime = std::move(CandidateRuntime);
			AdvanceGeneration(m_RuntimeUISessionGeneration);
			RetiredRuntime.reset();
			RetiredScene.reset();
		}

		auto Result = m_Runtime->Advance(*m_Scene, Timestep(m_Paused ? 0.0 : ScaledDeltaSeconds));
		m_Paused = m_Runtime->IsPaused();
		const auto BoundaryRequest = m_Runtime->GetPendingBoundaryRequest();
		if (m_StopRequested || (BoundaryRequest && BoundaryRequest->Kind == SceneRuntimeBoundaryRequestKind::Stop))
		{
			Stop();
			if (!Result)
				return std::unexpected(MakeError(
					PlayerSessionErrorCode::RuntimeOperationFailed, "Could not advance player scene runtime: " + Result.error().Message));
			return *Result;
		}
		if (BoundaryRequest && BoundaryRequest->Kind == SceneRuntimeBoundaryRequestKind::SceneTransition && BoundaryRequest->Scene)
		{
			const AssetID RequestedScene = *BoundaryRequest->Scene;
			m_RequestedSceneTransition.reset();
			if (auto Queued = RequestSceneTransition(RequestedScene); !Queued)
			{
				(void)m_Runtime->ResolveBoundaryRequest(*BoundaryRequest, SceneRuntimeBoundaryResolution::Rejected);
				return std::unexpected(MakeError(PlayerSessionErrorCode::SceneLoadFailed,
					"Could not prepare requested scene transition: " + Queued.error().Message));
			}
			m_PendingScene->OriginRequest = BoundaryRequest;
		}
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
		const bool HadRuntime = static_cast<bool>(m_Runtime);
		m_StopRequested = false;
		m_RequestedSceneTransition.reset();
		m_PendingScene.reset();
		m_Runtime.reset();
		if (HadRuntime) AdvanceGeneration(m_RuntimeUISessionGeneration);
		m_Scene.reset();
		m_AudioSession.reset();
		m_SceneAsset.reset();
#if !defined(PF_COOKED_RUNTIME_ONLY)
		m_Project.reset();
#endif
		m_Paused = false;
		m_TimeScale = 1.0;
	}
} // namespace PulseForge
