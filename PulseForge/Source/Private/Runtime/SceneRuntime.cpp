#include "Runtime/SceneRuntime.h"
#include "Runtime/CookedContentProvider.h"

#include "Assets/AssetPathResolver.h"
#include "Assets/AssetTypes.h"
#if !defined(PF_COOKED_RUNTIME_ONLY)
#include "Assets/Project.h"
#endif
#include "Audio/AudioSceneRuntime.h"
#include "Audio/AudioSession.h"
#include "Core/Diagnostics.h"
#include "Physics/PhysicsSceneRuntime.h"
#include "Scene/Scene.h"
#include "Scripting/ScriptRuntime.h"
#include "UI/RuntimeUI.h"

#include <optional>
#include <algorithm>
#include <array>
#include <exception>
#include <atomic>
#include <fstream>
#include <iterator>
#include <unordered_map>

namespace PulseForge
{
	namespace
	{
		SceneRuntimeError MakeError(
			SceneRuntimeErrorCode Code,
			SceneRuntimeSubsystem Subsystem,
			std::string Message)
		{
			return { Code, Subsystem, std::move(Message) };
		}

		std::string_view AudioDeviceStateName(AudioDeviceState State) noexcept
		{
			switch (State)
			{
				case AudioDeviceState::DefaultActive: return "default_active";
				case AudioDeviceState::ExplicitNull: return "explicit_null";
				case AudioDeviceState::Unavailable: return "unavailable";
				case AudioDeviceState::Recovering: return "recovering";
				case AudioDeviceState::RecoveryFailed: return "recovery_failed";
				case AudioDeviceState::OfflineTest: return "offline_test";
			}
			return "unknown";
		}

		uint64_t NextRuntimeSessionGeneration() noexcept
		{
			static std::atomic<uint64_t> Generation = 0;
			uint64_t Value = Generation.fetch_add(1, std::memory_order_relaxed) + 1;
			if (Value == 0) Value = Generation.fetch_add(1, std::memory_order_relaxed) + 1;
			return Value;
		}
		constexpr uint32_t MaximumBlueprintInstructionsPerFrame = 50'000;

		std::expected<std::string, std::string> ReadRuntimeUIDocumentBytes(
			const Project* SourceProject, const std::shared_ptr<const CookedContentProvider>& CookedAssets, const AssetID& Asset)
		{
			if (CookedAssets)
			{
				auto Bytes = CookedAssets->ReadRuntimeUI(Asset);
				if (!Bytes) return std::unexpected(Bytes.error().Message);
				return std::string(Bytes->empty() ? "" : reinterpret_cast<const char*>(Bytes->data()), Bytes->size());
			}
			if (!SourceProject) return std::unexpected(std::string("Runtime UI requires a source project or cooked content provider"));
#if !defined(PF_COOKED_RUNTIME_ONLY)
			const auto Record = SourceProject->GetAssetRegistry().Find(Asset);
			if (!Record || !HasAssetTypeExtension(Record->ProjectRelativePath, AssetFileType::RuntimeUI))
				return std::unexpected(std::string("Runtime UI document is missing from the managed project asset registry"));
			auto Path = AssetPathResolver::ResolveManagedSourcePath(SourceProject->GetRootPath(), Record->ProjectRelativePath);
			if (!Path) return std::unexpected(Path.error().Message);
			std::error_code Error;
			const uintmax_t Size = std::filesystem::file_size(*Path, Error);
			if (Error || Size == 0 || Size > 2u * 1024u * 1024u)
				return std::unexpected(std::string("Runtime UI document could not be read within the 2 MiB limit"));
			std::ifstream Input(*Path, std::ios::binary);
			if (!Input.is_open()) return std::unexpected(std::string("Runtime UI document could not be opened"));
			std::string Text(static_cast<size_t>(Size), '\0');
			Input.read(Text.data(), static_cast<std::streamsize>(Text.size()));
			if (!Input || Input.bad() || static_cast<size_t>(Input.gcount()) != Text.size())
				return std::unexpected(std::string("Runtime UI document could not be read completely"));
			return Text;
#else
			return std::unexpected(std::string("Cooked runtime cannot resolve source project UI documents"));
#endif
		}
	}

	struct SceneRuntime::Impl final
	{
		struct BoundBlueprintEvent final
		{
			Entity BoundEntity;
			bool Resolved = false;
			AssetID Asset;
			uint64_t Revision = 0;
			std::shared_ptr<const VerifiedBlueprintProgram> Program;
			bool Faulted = false;
		};
		struct BlueprintInstanceState final
		{
			Entity BoundEntity;
			AssetID Asset;
			uint64_t Revision = 0;
			std::vector<std::string> PropertyIds;
			std::vector<float> ScalarState;
		};
		static constexpr size_t BlueprintEventCount = static_cast<size_t>(BlueprintEventKind::OnUIAction) + 1;
		using BlueprintEventStates = std::array<BoundBlueprintEvent, BlueprintEventCount>;

		struct RuntimeUICanvasState final
		{
			Entity Owner;
			AssetID Asset;
			RuntimeUIInputPolicy InputPolicy = RuntimeUIInputPolicy::Passive;
			RuntimeUIDocument Document;
			RuntimeUIController Controller;
			std::vector<RuntimeUILayoutEntry> Layout;
			std::vector<RuntimeUIDrawCommand> DrawCommands;
			uint64_t TreeGeneration = 0;
		};
		// Reverse destruction stops scripts and scene voices before the session-owned engine and cache.
		std::unique_ptr<AudioSession> OwnedAudioSession;
		AudioSession* AudioSessionState = nullptr;
		std::unique_ptr<AudioSceneRuntime> Audio;
		std::unique_ptr<PhysicsSceneRuntime> Physics;
		std::unique_ptr<ScriptRuntime> Scripting;
		std::optional<RuntimeUICanvasState> RuntimeUI;
		std::unordered_map<UUID, BlueprintEventStates, UUIDHash> BlueprintEvents;
		std::unordered_map<UUID, std::unordered_map<AssetID, BlueprintInstanceState, UUIDHash>, UUIDHash> BlueprintInstances;
		std::unordered_map<UUID, Entity, UUIDHash> BlueprintKnownEntities;
		std::vector<BlueprintRuntimeDiagnostic> BlueprintDiagnostics;
		RuntimeUIViewport RuntimeUIView{};
		std::vector<RuntimeUIInputEvent> RuntimeUIEvents;
		std::vector<RuntimeUIDrawCommand> RuntimeUIDrawCommands;
		Scene* Source = nullptr;
		bool Paused = false;
		bool AudioActivationDeferred = false;
		enum class Lifecycle : uint8_t { Starting, Active, Stopping };
		enum class RequestStatus : uint8_t { None, Pending, Committed, Rejected, Cancelled, Superseded };
		Lifecycle State = Lifecycle::Starting;
		RequestStatus BoundaryRequestStatus = RequestStatus::None;
		std::optional<SceneRuntimeBoundaryRequest> BoundaryRequest;
		std::optional<bool> RequestedPause;
		uint64_t SessionGeneration = 0;
		uint64_t BlueprintProgramGeneration = 0;
		uint64_t NextBoundaryRequestGeneration = 0;
		uint32_t BlueprintInstructionsRemaining = MaximumBlueprintInstructionsPerFrame;
		uint32_t BlueprintInstructionsExecuted = 0;

		void RecordBlueprintDiagnostic(
			const Entity& Target,
			const BlueprintProgramBinding* Binding,
			BlueprintEventKind Event,
			BlueprintProgramErrorCode Code,
			UUID SourceNode,
			std::string Message)
		{
			if (BlueprintDiagnostics.size() == 256) BlueprintDiagnostics.erase(BlueprintDiagnostics.begin());
			BlueprintDiagnostics.push_back({ Target.GetUUID(), Binding ? Binding->Asset : AssetID{}, Event, Code,
				SourceNode, std::move(Message) });
		}

		void DispatchBlueprintEvent(
			Scene& SceneValue,
			const Entity& Target,
			BlueprintEventKind Event,
			const SceneRuntimeServices& Services)
		{
			if (!Services.ResolveBlueprintProgram || !Target.IsValid()) return;
			try
			{
				const auto Current = SceneValue.FindEntity(Target.GetUUID());
				if (!Current || *Current != Target) return;
				const size_t EventIndex = static_cast<size_t>(Event);
				if (EventIndex >= BlueprintEventCount)
				{
					RecordBlueprintDiagnostic(Target, nullptr, Event, BlueprintProgramErrorCode::InvalidOperand, {},
						"Blueprint event binding uses an unknown event");
					return;
				}

				BoundBlueprintEvent& Instance = BlueprintEvents[Target.GetUUID()][EventIndex];
				if (Instance.BoundEntity != Target)
				{
					Instance = {};
					Instance.BoundEntity = Target;
				}
				if (!Instance.Resolved)
				{
					const auto Binding = Services.ResolveBlueprintProgram(Target, Event);
					Instance.Resolved = true;
					if (!Binding || !Binding->Program) return;
					if (Binding->Asset.IsNil())
					{
						RecordBlueprintDiagnostic(Target, &*Binding, Event, BlueprintProgramErrorCode::InvalidOperand, {},
							"Blueprint event binding is missing a stable asset identity");
						Instance.Faulted = true;
						return;
					}
					Instance.Asset = Binding->Asset;
					Instance.Revision = Binding->Revision;
					Instance.Program = Binding->Program;
				}
				if (!Instance.Program) return;
				const BlueprintProgramBinding Binding{ Instance.Asset, Instance.Revision, Instance.Program };
				if (Instance.Faulted) return;
				if (BlueprintInstructionsRemaining == 0)
				{
					RecordBlueprintDiagnostic(Target, &Binding, Event, BlueprintProgramErrorCode::InstructionBudgetExceeded, {},
						"Scene Blueprint instruction budget was exhausted for this frame");
					Instance.Faulted = true;
					return;
				}
				BlueprintInstanceState& ProgramInstance = BlueprintInstances[Target.GetUUID()][Instance.Asset];
				const BlueprintProgramSource& ProgramSource = Instance.Program->GetSource();
				if (ProgramInstance.BoundEntity != Target || ProgramInstance.Asset != Instance.Asset ||
					ProgramInstance.Revision != Instance.Revision)
				{
					ProgramInstance = {};
					ProgramInstance.BoundEntity = Target;
					ProgramInstance.Asset = Instance.Asset;
					ProgramInstance.Revision = Instance.Revision;
					ProgramInstance.PropertyIds = ProgramSource.ScalarPropertyIds;
					ProgramInstance.ScalarState.resize(ProgramSource.ScalarPropertyIds.size(), 0.0f);
					if (Services.ReadBlueprintScalarProperty)
					{
						for (size_t PropertyIndex = 0; PropertyIndex < ProgramInstance.PropertyIds.size(); ++PropertyIndex)
							if (const auto InitialValue = Services.ReadBlueprintScalarProperty(Target, ProgramInstance.PropertyIds[PropertyIndex]))
								ProgramInstance.ScalarState[PropertyIndex] = *InitialValue;
					}
				}
				else if (ProgramInstance.PropertyIds != ProgramSource.ScalarPropertyIds)
				{
					RecordBlueprintDiagnostic(Target, &Binding, Event, BlueprintProgramErrorCode::InvalidOperand, {},
						"Blueprint event programs for one asset disagree on their scalar property schema");
					Instance.Faulted = true;
					return;
				}

				std::vector<float> CandidateState = ProgramInstance.ScalarState;
				const uint32_t ExecutionBudget = std::min(BlueprintInstructionsRemaining, BlueprintDefaultInstructionBudget);
				auto Result = ExecuteBlueprintProgram(*Instance.Program, CandidateState, ExecutionBudget);
				if (!Result)
				{
					if (Result.error().Code == BlueprintProgramErrorCode::InstructionBudgetExceeded)
					{
						BlueprintInstructionsRemaining -= ExecutionBudget;
						BlueprintInstructionsExecuted += ExecutionBudget;
					}
					RecordBlueprintDiagnostic(Target, &Binding, Event, Result.error().Code, Result.error().SourceNode,
						std::move(Result.error().Message));
					Instance.Faulted = true;
					return;
				}
				if (!Result->PropertyWrites.empty() &&
					(!Services.ApplyBlueprintPropertyWrites || !Services.ApplyBlueprintPropertyWrites(Target, Result->PropertyWrites)))
				{
					RecordBlueprintDiagnostic(Target, &Binding, Event, BlueprintProgramErrorCode::HostRejectedPropertyWrite,
						Result->PropertyWrites.front().SourceNode,
						"Scene runtime host rejected the Blueprint property transaction");
					Instance.Faulted = true;
					return;
				}
				BlueprintInstructionsRemaining -= std::min(BlueprintInstructionsRemaining, Result->InstructionsExecuted);
				BlueprintInstructionsExecuted += Result->InstructionsExecuted;
				ProgramInstance.ScalarState = std::move(CandidateState);
			}
			catch (const std::exception& Exception)
			{
				RecordBlueprintDiagnostic(Target, nullptr, Event, BlueprintProgramErrorCode::HostFailure, {}, Exception.what());
				const auto Found = BlueprintEvents.find(Target.GetUUID());
				if (Found != BlueprintEvents.end()) Found->second[static_cast<size_t>(Event)].Faulted = true;
			}
			catch (...)
			{
				RecordBlueprintDiagnostic(Target, nullptr, Event, BlueprintProgramErrorCode::HostFailure, {},
					"Blueprint host callback failed with a non-standard exception");
				const auto Found = BlueprintEvents.find(Target.GetUUID());
				if (Found != BlueprintEvents.end()) Found->second[static_cast<size_t>(Event)].Faulted = true;
			}
		}

		void ReconcileBlueprintEntities(Scene& SceneValue, const SceneRuntimeServices& Services)
		{
			std::unordered_map<UUID, Entity, UUIDHash> LiveEntities;
			const std::vector<Entity> Entities = SceneValue.GetEntities();
			LiveEntities.reserve(Entities.size());
			for (const Entity& EntityValue : Entities) LiveEntities.emplace(EntityValue.GetUUID(), EntityValue);

			for (auto Iterator = BlueprintKnownEntities.begin(); Iterator != BlueprintKnownEntities.end();)
			{
				const auto Live = LiveEntities.find(Iterator->first);
				if (Live == LiveEntities.end() || Live->second != Iterator->second)
				{
					BlueprintEvents.erase(Iterator->first);
					BlueprintInstances.erase(Iterator->first);
					Iterator = BlueprintKnownEntities.erase(Iterator);
				}
				else ++Iterator;
			}

			for (const Entity& EntityValue : Entities)
			{
				const UUID Identifier = EntityValue.GetUUID();
				const auto Known = BlueprintKnownEntities.find(Identifier);
				if (Known != BlueprintKnownEntities.end()) continue;
				BlueprintKnownEntities.emplace(Identifier, EntityValue);
				DispatchBlueprintEvent(SceneValue, EntityValue, BlueprintEventKind::OnCreate, Services);
			}
		}
	};

	#if !defined(PF_COOKED_RUNTIME_ONLY)
	SceneRuntime::SceneRuntime(
		const Project& SourceProject,
		SceneRuntimeDesc Description,
		SceneRuntimeServices Services)
		: m_Project(&SourceProject), m_Description(Description), m_Services(Services)
	{
	}
	#endif

	SceneRuntime::SceneRuntime(
		std::shared_ptr<const CookedContentProvider> CookedAssets,
		SceneRuntimeDesc Description,
		SceneRuntimeServices Services)
		: m_CookedAssets(std::move(CookedAssets)), m_Description(Description), m_Services(Services)
	{
	}

	SceneRuntime::~SceneRuntime()
	{
		Stop();
	}

	std::expected<void, SceneRuntimeError> SceneRuntime::Start(Scene& Source, bool DeferAudioActivation)
	{
		if (m_Impl)
			return std::unexpected(MakeError(
				SceneRuntimeErrorCode::AlreadyRunning,
				SceneRuntimeSubsystem::None,
				"Scene runtime is already started"));

		try
		{
			const uint64_t SessionGeneration = NextRuntimeSessionGeneration();
			m_Description.Scripting.SessionGeneration = SessionGeneration;
			m_Description.Physics.SessionGeneration = SessionGeneration;
			auto Candidate = std::make_unique<Impl>();
			Candidate->Source = &Source;
			Candidate->SessionGeneration = SessionGeneration;
			Candidate->AudioActivationDeferred = DeferAudioActivation;
			const Project* SourceProject = nullptr;
#if !defined(PF_COOKED_RUNTIME_ONLY)
			SourceProject = m_Project;
#endif
			for (const Entity& EntityValue : Source.GetEntities())
			{
				auto Canvas = EntityValue.GetRuntimeUICanvas();
				if (!Canvas)
					return std::unexpected(MakeError(SceneRuntimeErrorCode::InitializationFailed, SceneRuntimeSubsystem::None,
						"Could not inspect runtime UI canvas: " + Canvas.error().Message));
				if (!Canvas->has_value() || !(**Canvas).Enabled) continue;
				if (Candidate->RuntimeUI)
					return std::unexpected(MakeError(SceneRuntimeErrorCode::InitializationFailed, SceneRuntimeSubsystem::None,
						"A scene may contain only one enabled screen-space Runtime UI Canvas"));
				auto Text = ReadRuntimeUIDocumentBytes(SourceProject, m_CookedAssets, (**Canvas).DocumentAsset);
				if (!Text)
					return std::unexpected(MakeError(SceneRuntimeErrorCode::InitializationFailed, SceneRuntimeSubsystem::None,
						"Could not load Runtime UI document " + (**Canvas).DocumentAsset.ToString() + ": " + Text.error()));
				auto Document = ParseRuntimeUIDocument(*Text);
				if (!Document)
					return std::unexpected(MakeError(SceneRuntimeErrorCode::InitializationFailed, SceneRuntimeSubsystem::None,
						"Runtime UI document failed validation: " + Document.error().Message));
				Candidate->RuntimeUI.emplace(Impl::RuntimeUICanvasState{
					.Owner = EntityValue, .Asset = (**Canvas).DocumentAsset, .InputPolicy = (**Canvas).InputPolicy,
					.Document = std::move(*Document),
					.TreeGeneration = SessionGeneration });
				Candidate->RuntimeUIView = { .LogicalSize = Candidate->RuntimeUI->Document.DesignSize,
					.FramebufferWidth = static_cast<uint32_t>(Candidate->RuntimeUI->Document.DesignSize.x),
					.FramebufferHeight = static_cast<uint32_t>(Candidate->RuntimeUI->Document.DesignSize.y) };
			}

			Candidate->AudioSessionState = m_Services.AudioSessionState;
			if (!Candidate->AudioSessionState)
			{
				AudioSessionDesc AudioDescription{ .Engine = m_Description.Audio, .Diagnostics = m_Services.Diagnostics };
				std::expected<std::unique_ptr<AudioSession>, AudioSessionError> CreatedSession =
					std::unexpected(AudioSessionError{ AudioSessionErrorCode::AssetProviderUnavailable,
						"Scene runtime requires a source project or cooked content provider" });
				if (m_CookedAssets)
					CreatedSession = AudioSession::CreateForCooked(
						m_CookedAssets, m_CookedAssets->GetAudioMixDefaults(), std::move(AudioDescription));
#if !defined(PF_COOKED_RUNTIME_ONLY)
				else if (m_Project)
					CreatedSession = AudioSession::CreateForProject(*m_Project, std::move(AudioDescription));
#endif
				if (!CreatedSession)
					return std::unexpected(MakeError(SceneRuntimeErrorCode::InitializationFailed,
						SceneRuntimeSubsystem::AudioDevice, "Could not create audio session: " + CreatedSession.error().Message));
				Candidate->OwnedAudioSession = std::move(*CreatedSession);
				Candidate->AudioSessionState = Candidate->OwnedAudioSession.get();
			}
			const UUID* ExpectedProjectID = nullptr;
			if (m_CookedAssets)
				ExpectedProjectID = &m_CookedAssets->GetProjectID();
#if !defined(PF_COOKED_RUNTIME_ONLY)
			else if (m_Project)
				ExpectedProjectID = &m_Project->GetDescription().ID;
#endif
			if (!Candidate->AudioSessionState || !ExpectedProjectID ||
				Candidate->AudioSessionState->GetProjectID() != *ExpectedProjectID)
				return std::unexpected(MakeError(SceneRuntimeErrorCode::InitializationFailed,
					SceneRuntimeSubsystem::AudioDevice,
					"Audio session identity does not match the active source project or cooked package"));

			Candidate->Physics = std::make_unique<PhysicsSceneRuntime>(m_Description.Physics);
			if (auto Result = Candidate->Physics->Start(Source); !Result)
				return std::unexpected(MakeError(
					SceneRuntimeErrorCode::InitializationFailed,
					SceneRuntimeSubsystem::Physics,
					"Could not start scene physics: " + Result.error().Message));

			Candidate->Audio = std::make_unique<AudioSceneRuntime>(
				Candidate->AudioSessionState->GetEngine(),
				Candidate->AudioSessionState->GetAssets(),
				Candidate->AudioSessionState->GetGeneration(),
				SessionGeneration);
			if (auto Result = Candidate->Audio->Start(Source, DeferAudioActivation); !Result)
				return std::unexpected(MakeError(
					SceneRuntimeErrorCode::InitializationFailed,
					SceneRuntimeSubsystem::Audio,
					"Could not start scene audio: " + Result.error().Message));

			ScriptRuntimeServices Services{
				.Physics = Candidate->Physics.get(),
				.Audio = Candidate->Audio.get(),
				.GameplayInput = m_Services.GameplayInput,
				.InputState = m_Services.InputState,
				.Observer = m_Services.ScriptObserver };
			auto* CandidateOwner = Candidate.get();
			const auto AudioSettingsAvailable = [CandidateOwner]() noexcept
			{
				return CandidateOwner->State == Impl::Lifecycle::Active && !CandidateOwner->AudioActivationDeferred &&
					CandidateOwner->AudioSessionState;
			};
			Services.GetAudioGain = [CandidateOwner, AudioSettingsAvailable](std::string_view Channel) -> std::optional<float>
			{
				if (!AudioSettingsAvailable()) return std::nullopt;
				const AudioMixState Mix = CandidateOwner->AudioSessionState->GetMixState();
				if (Channel == "master") return Mix.Gains.Master;
				if (Channel == "music") return Mix.Gains.Music;
				if (Channel == "effects") return Mix.Gains.Effects;
				if (Channel == "ui") return Mix.Gains.UI;
				return std::nullopt;
			};
			Services.SetAudioGain = [CandidateOwner, AudioSettingsAvailable](std::string_view Channel, float Gain)
			{
				if (!AudioSettingsAvailable()) return false;
				std::expected<void, AudioSessionError> Result = std::unexpected(AudioSessionError{
					AudioSessionErrorCode::InvalidMixSettings, "Unknown audio mix channel" });
				if (Channel == "master") Result = CandidateOwner->AudioSessionState->SetUserMasterGain(Gain);
				else if (Channel == "music") Result = CandidateOwner->AudioSessionState->SetUserBusGain(AudioBus::Music, Gain);
				else if (Channel == "effects") Result = CandidateOwner->AudioSessionState->SetUserBusGain(AudioBus::Effects, Gain);
				else if (Channel == "ui") Result = CandidateOwner->AudioSessionState->SetUserBusGain(AudioBus::UI, Gain);
				return Result.has_value();
			};
			Services.GetAudioMuted = [CandidateOwner, AudioSettingsAvailable](std::string_view Channel) -> std::optional<bool>
			{
				if (!AudioSettingsAvailable()) return std::nullopt;
				const AudioMixState Mix = CandidateOwner->AudioSessionState->GetMixState();
				if (Channel == "master") return Mix.MasterMuted;
				if (Channel == "music") return Mix.MusicMuted;
				if (Channel == "effects") return Mix.EffectsMuted;
				if (Channel == "ui") return Mix.UIMuted;
				return std::nullopt;
			};
			Services.SetAudioMuted = [CandidateOwner, AudioSettingsAvailable](std::string_view Channel, bool Muted)
			{
				if (!AudioSettingsAvailable()) return false;
				std::expected<void, AudioSessionError> Result = std::unexpected(AudioSessionError{
					AudioSessionErrorCode::InvalidMixSettings, "Unknown audio mix channel" });
				if (Channel == "master") Result = CandidateOwner->AudioSessionState->SetMasterMuted(Muted);
				else if (Channel == "music") Result = CandidateOwner->AudioSessionState->SetBusMuted(AudioBus::Music, Muted);
				else if (Channel == "effects") Result = CandidateOwner->AudioSessionState->SetBusMuted(AudioBus::Effects, Muted);
				else if (Channel == "ui") Result = CandidateOwner->AudioSessionState->SetBusMuted(AudioBus::UI, Muted);
				return Result.has_value();
			};
			Services.SaveAudioSettings = [CandidateOwner, AudioSettingsAvailable]()
			{
				return AudioSettingsAvailable() && CandidateOwner->AudioSessionState->SaveUserSettings().has_value();
			};
			Services.GetAudioOutputState = [CandidateOwner, AudioSettingsAvailable]() -> std::optional<std::string_view>
			{
				if (!AudioSettingsAvailable()) return std::nullopt;
				return AudioDeviceStateName(CandidateOwner->AudioSessionState->GetDeviceState());
			};
			Services.RetryAudioOutput = [CandidateOwner, AudioSettingsAvailable]()
			{
				return AudioSettingsAvailable() && CandidateOwner->AudioSessionState->RetryDefaultOutput().has_value();
			};
			if (m_Services.QueueSceneTransition)
			{
				Services.QueueSceneTransition = [CandidateOwner, Request = m_Services.QueueSceneTransition](const AssetID& Asset)
				{
					if (CandidateOwner->State != Impl::Lifecycle::Active ||
						CandidateOwner->BoundaryRequestStatus == Impl::RequestStatus::Committed) return false;
					if (CandidateOwner->BoundaryRequest &&
						CandidateOwner->BoundaryRequest->Kind == SceneRuntimeBoundaryRequestKind::Stop) return false;
					if (CandidateOwner->BoundaryRequest) return false;
					if (!Request(Asset)) return false;
					if (++CandidateOwner->NextBoundaryRequestGeneration == 0)
						++CandidateOwner->NextBoundaryRequestGeneration;
					CandidateOwner->BoundaryRequest = SceneRuntimeBoundaryRequest{
						.SessionGeneration = CandidateOwner->SessionGeneration,
						.RequestGeneration = CandidateOwner->NextBoundaryRequestGeneration,
						.Kind = SceneRuntimeBoundaryRequestKind::SceneTransition,
						.Scene = Asset };
					CandidateOwner->BoundaryRequestStatus = Impl::RequestStatus::Pending;
					return true;
				};
			}
			if (m_Services.QueueStop)
			{
				Services.QueueStop = [CandidateOwner, Request = m_Services.QueueStop]()
				{
					if (CandidateOwner->State != Impl::Lifecycle::Active ||
						CandidateOwner->BoundaryRequestStatus == Impl::RequestStatus::Committed) return false;
					if (CandidateOwner->BoundaryRequest &&
						CandidateOwner->BoundaryRequest->Kind == SceneRuntimeBoundaryRequestKind::Stop) return false;
					if (!Request()) return false;
					if (CandidateOwner->BoundaryRequest)
						CandidateOwner->BoundaryRequestStatus = Impl::RequestStatus::Superseded;
					if (++CandidateOwner->NextBoundaryRequestGeneration == 0)
						++CandidateOwner->NextBoundaryRequestGeneration;
					CandidateOwner->BoundaryRequest = SceneRuntimeBoundaryRequest{
						.SessionGeneration = CandidateOwner->SessionGeneration,
						.RequestGeneration = CandidateOwner->NextBoundaryRequestGeneration,
						.Kind = SceneRuntimeBoundaryRequestKind::Stop };
					CandidateOwner->BoundaryRequestStatus = Impl::RequestStatus::Pending;
					return true;
				};
			}
			Services.QueuePause = [CandidateOwner](bool Paused)
			{
				if (CandidateOwner->State != Impl::Lifecycle::Active || CandidateOwner->BoundaryRequest ||
					CandidateOwner->BoundaryRequestStatus == Impl::RequestStatus::Committed)
					return false;
				CandidateOwner->RequestedPause = Paused;
				return true;
			};
			Services.SetRuntimeUIText = [CandidateOwner](UUID ElementID, std::string Text)
			{
				if (CandidateOwner->State != Impl::Lifecycle::Active || !CandidateOwner->RuntimeUI ||
					Text.size() > RuntimeUIDocument::MaximumTextBytes)
					return false;
				RuntimeUIDocument CandidateDocument = CandidateOwner->RuntimeUI->Document;
				const auto Element = std::find_if(CandidateDocument.Elements.begin(), CandidateDocument.Elements.end(),
					[ElementID](const RuntimeUIElement& Value) { return Value.ID == ElementID; });
				if (Element == CandidateDocument.Elements.end() ||
					(Element->Kind != RuntimeUIElementKind::Text && Element->Kind != RuntimeUIElementKind::Button))
					return false;
				Element->Text = std::move(Text);
				if (!ValidateRuntimeUIDocument(CandidateDocument)) return false;
				CandidateOwner->RuntimeUI->Document = std::move(CandidateDocument);
				return true;
			};
			if (m_CookedAssets)
				Candidate->Scripting = std::make_unique<ScriptRuntime>(m_CookedAssets, m_Description.Scripting, Services);
#if !defined(PF_COOKED_RUNTIME_ONLY)
			else
				Candidate->Scripting = std::make_unique<ScriptRuntime>(*m_Project, m_Description.Scripting, Services);
#endif
			if (auto Result = Candidate->Scripting->Start(Source); !Result)
				return std::unexpected(MakeError(
					SceneRuntimeErrorCode::InitializationFailed,
					SceneRuntimeSubsystem::Scripting,
					"Could not start scene scripting: " + Result.error().Message));

			Candidate->State = Impl::Lifecycle::Active;
			Candidate->BlueprintInstructionsRemaining = MaximumBlueprintInstructionsPerFrame;
			Candidate->BlueprintInstructionsExecuted = 0;
			if (m_Services.GetBlueprintProgramGeneration)
				Candidate->BlueprintProgramGeneration = m_Services.GetBlueprintProgramGeneration();
			for (const Entity& EntityValue : Source.GetEntities())
			{
				Candidate->BlueprintKnownEntities.emplace(EntityValue.GetUUID(), EntityValue);
				Candidate->DispatchBlueprintEvent(Source, EntityValue, BlueprintEventKind::OnCreate, m_Services);
			}
			m_Impl = std::move(Candidate);
			return {};
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(MakeError(
				SceneRuntimeErrorCode::InitializationFailed,
				SceneRuntimeSubsystem::None,
				std::string("Could not start scene runtime: ") + Exception.what()));
		}
	}

	std::expected<void, SceneRuntimeError> SceneRuntime::ActivateAudio()
	{
		if (!m_Impl)
			return std::unexpected(MakeError(SceneRuntimeErrorCode::NotRunning, SceneRuntimeSubsystem::None,
				"Scene runtime is not started"));
		if (!m_Impl->AudioActivationDeferred)
			return {};
		if (auto Activated = m_Impl->Audio->Activate(); !Activated)
			return std::unexpected(MakeError(SceneRuntimeErrorCode::UpdateFailed, SceneRuntimeSubsystem::Audio,
				"Could not activate candidate scene audio: " + Activated.error().Message));
		m_Impl->AudioActivationDeferred = false;
		return {};
	}

	std::expected<void, SceneRuntimeError> SceneRuntime::SuspendAudioForTransition()
	{
		if (!m_Impl)
			return std::unexpected(MakeError(SceneRuntimeErrorCode::NotRunning, SceneRuntimeSubsystem::None,
				"Scene runtime is not started"));
		if (auto Suspended = m_Impl->Audio->SuspendForTransition(); !Suspended)
			return std::unexpected(MakeError(SceneRuntimeErrorCode::UpdateFailed, SceneRuntimeSubsystem::Audio,
				"Could not suspend outgoing scene audio: " + Suspended.error().Message));
		return {};
	}

	std::expected<void, SceneRuntimeError> SceneRuntime::ResumeAudioAfterTransition()
	{
		if (!m_Impl)
			return std::unexpected(MakeError(SceneRuntimeErrorCode::NotRunning, SceneRuntimeSubsystem::None,
				"Scene runtime is not started"));
		if (auto Resumed = m_Impl->Audio->ResumeAfterTransition(); !Resumed)
			return std::unexpected(MakeError(SceneRuntimeErrorCode::UpdateFailed, SceneRuntimeSubsystem::Audio,
				"Could not resume retained scene audio: " + Resumed.error().Message));
		return {};
	}

	void SceneRuntime::Stop() noexcept
	{
		if (m_Impl)
		{
			m_Impl->State = Impl::Lifecycle::Stopping;
			m_Impl->BoundaryRequestStatus = Impl::RequestStatus::Cancelled;
			m_Impl->BoundaryRequest.reset();
		}
		m_Impl.reset();
	}

	bool SceneRuntime::IsRunning() const noexcept
	{
		return m_Impl != nullptr;
	}

	std::expected<void, SceneRuntimeError> SceneRuntime::SetPaused(bool Paused)
	{
		if (!m_Impl)
			return std::unexpected(
				MakeError(SceneRuntimeErrorCode::NotRunning, SceneRuntimeSubsystem::None, "Scene runtime is not started"));
		if (m_Impl->Paused == Paused) return {};
		if (m_Services.AudioPauseGuard)
		{
			try
			{
				if (const auto Rejection = m_Services.AudioPauseGuard(Paused))
					return std::unexpected(MakeError(SceneRuntimeErrorCode::UpdateFailed, SceneRuntimeSubsystem::Audio,
						"Audio pause change was rejected by the host: " + *Rejection));
			}
			catch (const std::exception& Exception)
			{
				return std::unexpected(MakeError(SceneRuntimeErrorCode::UpdateFailed, SceneRuntimeSubsystem::Audio,
					std::string("Audio pause guard failed: ") + Exception.what()));
			}
			catch (...)
			{
				return std::unexpected(MakeError(SceneRuntimeErrorCode::UpdateFailed, SceneRuntimeSubsystem::Audio,
					"Audio pause guard failed with an unknown exception"));
			}
		}
		if (auto Result = m_Impl->Audio->SetPaused(Paused); !Result)
			return std::unexpected(MakeError(SceneRuntimeErrorCode::UpdateFailed, SceneRuntimeSubsystem::Audio,
				"Could not change scene audio pause state: " + Result.error().Message));
		m_Impl->Paused = Paused;
		return {};
	}

	bool SceneRuntime::IsPaused() const noexcept
	{
		return m_Impl && m_Impl->Paused;
	}

	bool SceneRuntime::HasRuntimeUI() const noexcept
	{
		return m_Impl && m_Impl->RuntimeUI.has_value();
	}

	RuntimeUIInputPolicy SceneRuntime::GetRuntimeUIInputPolicy() const noexcept
	{
		if (!m_Impl || !m_Impl->RuntimeUI) return RuntimeUIInputPolicy::Passive;
		return m_Impl->Paused ? RuntimeUIInputPolicy::Modal : m_Impl->RuntimeUI->InputPolicy;
	}

	uint64_t SceneRuntime::GetRuntimeUIInputGeneration() const noexcept
	{
		return m_Impl && m_Impl->RuntimeUI ? m_Impl->RuntimeUI->TreeGeneration : 0;
	}

	bool SceneRuntime::SubmitRuntimeUIFrame(RuntimeUIViewport Viewport, std::span<const RuntimeUIInputEvent> Events)
	{
		if (!m_Impl || !m_Impl->RuntimeUI || Events.size() > 512) return false;
		m_Impl->RuntimeUIView = Viewport;
		m_Impl->RuntimeUIEvents.assign(Events.begin(), Events.end());
		return true;
	}

	std::span<const RuntimeUIDrawCommand> SceneRuntime::GetRuntimeUIDrawCommands() const noexcept
	{
		return m_Impl ? std::span<const RuntimeUIDrawCommand>(m_Impl->RuntimeUIDrawCommands) :
			std::span<const RuntimeUIDrawCommand>{};
	}

	RuntimeUIViewport SceneRuntime::GetRuntimeUIViewport() const noexcept
	{
		return m_Impl ? m_Impl->RuntimeUIView : RuntimeUIViewport{};
	}

	std::expected<uint64_t, SceneRuntimeError> SceneRuntime::RequestScriptReload(const Entity& Target)
	{
		if (!m_Impl)
			return std::unexpected(MakeError(SceneRuntimeErrorCode::NotRunning, SceneRuntimeSubsystem::Scripting,
				"Script reload requires an active Play or Player session"));
		auto Queued = m_Impl->Scripting->RequestReload(Target);
		if (!Queued)
			return std::unexpected(MakeError(SceneRuntimeErrorCode::UpdateFailed, SceneRuntimeSubsystem::Scripting,
				Queued.error().Message));
		return *Queued;
	}

	std::span<const ScriptDiagnostic> SceneRuntime::GetScriptDiagnostics() const noexcept
	{
		return m_Impl ? m_Impl->Scripting->GetDiagnostics() : std::span<const ScriptDiagnostic>{};
	}

	std::span<const BlueprintRuntimeDiagnostic> SceneRuntime::GetBlueprintDiagnostics() const noexcept
	{
		return m_Impl ? std::span<const BlueprintRuntimeDiagnostic>(m_Impl->BlueprintDiagnostics) :
			std::span<const BlueprintRuntimeDiagnostic>{};
	}

	std::optional<SceneRuntimeBoundaryRequest> SceneRuntime::GetPendingBoundaryRequest() const noexcept
	{
		if (!m_Impl || m_Impl->BoundaryRequestStatus != Impl::RequestStatus::Pending) return std::nullopt;
		return m_Impl->BoundaryRequest;
	}

	bool SceneRuntime::ResolveBoundaryRequest(
		const SceneRuntimeBoundaryRequest& Request,
		SceneRuntimeBoundaryResolution Resolution) noexcept
	{
		if (!m_Impl || m_Impl->State != Impl::Lifecycle::Active ||
			m_Impl->BoundaryRequestStatus != Impl::RequestStatus::Pending || !m_Impl->BoundaryRequest ||
			m_Impl->BoundaryRequest->SessionGeneration != Request.SessionGeneration ||
			m_Impl->BoundaryRequest->RequestGeneration != Request.RequestGeneration ||
			m_Impl->BoundaryRequest->Kind != Request.Kind || m_Impl->BoundaryRequest->Scene != Request.Scene)
			return false;

		switch (Resolution)
		{
			case SceneRuntimeBoundaryResolution::Committed:
				m_Impl->BoundaryRequestStatus = Impl::RequestStatus::Committed;
				return true;
			case SceneRuntimeBoundaryResolution::Rejected:
				m_Impl->BoundaryRequestStatus = Impl::RequestStatus::Rejected;
				m_Impl->BoundaryRequest.reset();
				return true;
			case SceneRuntimeBoundaryResolution::Cancelled:
				m_Impl->BoundaryRequestStatus = Impl::RequestStatus::Cancelled;
				m_Impl->BoundaryRequest.reset();
				return true;
		}
		return false;
	}

	std::expected<SceneRuntimeFrameResult, SceneRuntimeError> SceneRuntime::Advance(
		Scene& Source,
		Timestep FrameDelta)
	{
		if (!m_Impl)
			return std::unexpected(MakeError(
				SceneRuntimeErrorCode::NotRunning,
				SceneRuntimeSubsystem::None,
				"Scene runtime is not started"));
		if (m_Impl->Source != &Source)
			return std::unexpected(MakeError(
				SceneRuntimeErrorCode::DifferentScene,
				SceneRuntimeSubsystem::None,
				"Scene runtime was started for a different Scene"));
		const double DeltaSeconds = FrameDelta.GetSeconds();
		if (!std::isfinite(DeltaSeconds) || DeltaSeconds < 0.0)
			return std::unexpected(MakeError(
				SceneRuntimeErrorCode::InvalidDeltaTime,
				SceneRuntimeSubsystem::None,
				"Scene runtime frame delta must be finite and non-negative"));
		if (m_Services.GetBlueprintProgramGeneration)
		{
			uint64_t CurrentProgramGeneration = 0;
			try
			{
				CurrentProgramGeneration = m_Services.GetBlueprintProgramGeneration();
			}
			catch (const std::exception& Exception)
			{
				return std::unexpected(MakeError(SceneRuntimeErrorCode::UpdateFailed, SceneRuntimeSubsystem::Scripting,
					std::string("Could not read Blueprint catalog generation: ") + Exception.what()));
			}
			catch (...)
			{
				return std::unexpected(MakeError(SceneRuntimeErrorCode::UpdateFailed, SceneRuntimeSubsystem::Scripting,
					"Could not read Blueprint catalog generation"));
			}
			if (CurrentProgramGeneration != m_Impl->BlueprintProgramGeneration)
			{
				m_Impl->BlueprintEvents.clear();
				m_Impl->BlueprintInstances.clear();
				m_Impl->BlueprintProgramGeneration = CurrentProgramGeneration;
			}
		}
		m_Impl->BlueprintInstructionsRemaining = MaximumBlueprintInstructionsPerFrame;
		m_Impl->BlueprintInstructionsExecuted = 0;
		if (m_Impl->AudioSessionState)
			m_Impl->AudioSessionState->PollDeviceState();
		if (m_Impl->BoundaryRequestStatus == Impl::RequestStatus::Pending ||
			m_Impl->BoundaryRequestStatus == Impl::RequestStatus::Committed)
			return SceneRuntimeFrameResult{ .PhysicsStats = m_Impl->Physics->GetStats() };
		m_Impl->ReconcileBlueprintEntities(Source, m_Services);
		if (auto Result = m_Impl->Scripting->ServicePendingReloads(Source); !Result)
			return std::unexpected(MakeError(SceneRuntimeErrorCode::UpdateFailed, SceneRuntimeSubsystem::Scripting,
				"Could not service queued script reloads: " + Result.error().Message));
		if (m_Impl->RuntimeUI)
		{
			auto& Canvas = *m_Impl->RuntimeUI;
			auto Layout = Canvas.Controller.Layout(Canvas.Document, m_Impl->RuntimeUIView, Canvas.TreeGeneration);
			if (!Layout)
				return std::unexpected(MakeError(SceneRuntimeErrorCode::UpdateFailed, SceneRuntimeSubsystem::None,
					"Could not lay out Runtime UI: " + Layout.error().Message));
			Canvas.Layout = std::move(*Layout);
			const std::vector<RuntimeUIAction> Actions = Canvas.Controller.ProcessInput(
				Canvas.Document, Canvas.Layout, m_Impl->RuntimeUIEvents, Canvas.TreeGeneration);
			Canvas.DrawCommands = BuildRuntimeUIDrawList(Canvas.Document, Canvas.Layout,
				Canvas.Controller.GetFocusedElement(), Canvas.Controller.GetHoveredElement());
			m_Impl->RuntimeUIEvents.clear();
			m_Impl->RuntimeUIDrawCommands = Canvas.DrawCommands;
			const auto CurrentOwner = Source.FindEntity(Canvas.Owner.GetUUID());
			if (CurrentOwner && *CurrentOwner == Canvas.Owner)
				for (const RuntimeUIAction& Action : Actions)
				{
					(void)m_Impl->Scripting->DispatchRuntimeUIAction(Source, Canvas.Owner, Action.Name);
					m_Impl->DispatchBlueprintEvent(Source, Canvas.Owner, BlueprintEventKind::OnUIAction, m_Services);
					if (m_Impl->BoundaryRequestStatus == Impl::RequestStatus::Pending ||
						m_Impl->BoundaryRequestStatus == Impl::RequestStatus::Committed)
						return SceneRuntimeFrameResult{ .PhysicsStats = m_Impl->Physics->GetStats() };
				}
		}
		else
			m_Impl->RuntimeUIDrawCommands.clear();
		if (m_Impl->BoundaryRequestStatus != Impl::RequestStatus::Pending &&
			m_Impl->BoundaryRequestStatus != Impl::RequestStatus::Committed)
		{
			if (auto Result = m_Impl->Audio->ProcessPendingCommands(); !Result)
				return std::unexpected(MakeError(SceneRuntimeErrorCode::UpdateFailed, SceneRuntimeSubsystem::Audio,
					"Could not apply queued audio controls: " + Result.error().Message));
		}
		if (m_Impl->RequestedPause && m_Impl->BoundaryRequestStatus != Impl::RequestStatus::Pending &&
			m_Impl->BoundaryRequestStatus != Impl::RequestStatus::Committed)
		{
			const bool RequestedPause = *m_Impl->RequestedPause;
			m_Impl->RequestedPause.reset();
			if (auto Result = SetPaused(RequestedPause); !Result) return std::unexpected(Result.error());
		}

		if (m_Impl->Paused) return SceneRuntimeFrameResult{};

		{
			ScopedDiagnosticTimer Timer(m_Services.Diagnostics, "ScriptRuntime::Advance", "ScriptRuntime");
			if (auto Result = m_Impl->Scripting->Advance(Source, FrameDelta); !Result)
				return std::unexpected(MakeError(
					SceneRuntimeErrorCode::UpdateFailed,
					SceneRuntimeSubsystem::Scripting,
					"Could not update scene scripts: " + Result.error().Message));
		}
		if (m_Impl->BoundaryRequestStatus == Impl::RequestStatus::Pending ||
			m_Impl->BoundaryRequestStatus == Impl::RequestStatus::Committed)
			return SceneRuntimeFrameResult{ .PhysicsStats = m_Impl->Physics->GetStats() };
		for (const Entity& EntityValue : Source.GetEntities())
			m_Impl->DispatchBlueprintEvent(Source, EntityValue, BlueprintEventKind::OnUpdate, m_Services);
		if (auto Result = m_Impl->Audio->ProcessPendingCommands(); !Result)
			return std::unexpected(MakeError(SceneRuntimeErrorCode::UpdateFailed, SceneRuntimeSubsystem::Audio,
				"Could not apply queued audio controls: " + Result.error().Message));
		if (m_Impl->RequestedPause)
		{
			const bool RequestedPause = *m_Impl->RequestedPause;
			m_Impl->RequestedPause.reset();
			if (auto Result = SetPaused(RequestedPause); !Result) return std::unexpected(Result.error());
			if (m_Impl->Paused) return SceneRuntimeFrameResult{ .PhysicsStats = m_Impl->Physics->GetStats() };
		}

		auto PhysicsResult = [&]
		{
			ScopedDiagnosticTimer Timer(m_Services.Diagnostics, "PhysicsSceneRuntime::Advance", "PhysicsSceneRuntime");
			return m_Impl->Physics->Advance(Source, FrameDelta);
		}();
		if (!PhysicsResult)
			return std::unexpected(MakeError(
				SceneRuntimeErrorCode::UpdateFailed,
				SceneRuntimeSubsystem::Physics,
				"Could not advance scene physics: " + PhysicsResult.error().Message));

		for (const PhysicsContactEvent& Event : m_Impl->Physics->TakeContactEvents())
		{
			if (!m_Impl->Scripting->DispatchPhysicsContact(Source, Event))
				return SceneRuntimeFrameResult{ .PhysicsSteps = *PhysicsResult, .PhysicsStats = m_Impl->Physics->GetStats() };
			const BlueprintEventKind BlueprintEvent = Event.IsSensorPair
				? Event.Phase == PhysicsContactPhase::Begin ? BlueprintEventKind::OnTriggerEnter
					: Event.Phase == PhysicsContactPhase::Stay ? BlueprintEventKind::OnTriggerStay : BlueprintEventKind::OnTriggerExit
				: Event.Phase == PhysicsContactPhase::Begin ? BlueprintEventKind::OnCollisionBegin
					: Event.Phase == PhysicsContactPhase::Stay ? BlueprintEventKind::OnCollisionStay : BlueprintEventKind::OnCollisionEnd;
			m_Impl->DispatchBlueprintEvent(Source, Event.First, BlueprintEvent, m_Services);
			m_Impl->DispatchBlueprintEvent(Source, Event.Second, BlueprintEvent, m_Services);
		}
		if (auto Result = m_Impl->Audio->ProcessPendingCommands(); !Result)
			return std::unexpected(MakeError(SceneRuntimeErrorCode::UpdateFailed, SceneRuntimeSubsystem::Audio,
				"Could not apply queued contact audio controls: " + Result.error().Message));

		{
			ScopedDiagnosticTimer Timer(m_Services.Diagnostics, "AudioSceneRuntime::Advance", "AudioSceneRuntime");
			if (auto Result = m_Impl->Audio->Advance(Source); !Result)
				return std::unexpected(MakeError(
					SceneRuntimeErrorCode::UpdateFailed,
					SceneRuntimeSubsystem::Audio,
					"Could not update scene audio: " + Result.error().Message));
		}

		return SceneRuntimeFrameResult{ *PhysicsResult, m_Impl->Physics->GetStats(), m_Impl->BlueprintInstructionsExecuted };
	}
}
