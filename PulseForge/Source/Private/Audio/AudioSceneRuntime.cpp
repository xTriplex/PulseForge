#include "Audio/AudioSceneRuntime.h"

#include "Assets/AudioAssetCache.h"
#include "Audio/AudioEngine.h"
#include "Scene/Components/AudioListenerComponent.h"
#include "Scene/Components/AudioSourceComponent.h"
#include "Scene/Entity.h"
#include "Scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <optional>
#include <unordered_map>
#include <vector>
#include <utility>

namespace PulseForge
{
	namespace
	{
		AudioSceneRuntimeError MakeError(AudioSceneRuntimeErrorCode Code, std::string Message)
		{
			return { Code, std::move(Message) };
		}

		AudioSceneRuntimeError SceneOperationError(const SceneError& Error)
		{
			return MakeError(AudioSceneRuntimeErrorCode::SceneOperationFailed, Error.Message);
		}

		bool IsFinite(const glm::vec3& Value)
		{
			return std::isfinite(Value.x) && std::isfinite(Value.y) && std::isfinite(Value.z);
		}

		struct ListenerPose
		{
			glm::vec3 Position;
			glm::vec3 Direction;
		};

		std::expected<ListenerPose, AudioSceneRuntimeError> GetListenerPose(const Entity& Listener)
		{
			const auto World = Listener.GetWorldMatrix();
			if (!World)
				return std::unexpected(SceneOperationError(World.error()));

			const glm::vec3 Position((*World)[3]);
			const glm::vec3 Direction(-(*World)[2]);
			const float DirectionLength = glm::length(Direction);
			if (!IsFinite(Position) || !IsFinite(Direction) || !std::isfinite(DirectionLength) || DirectionLength <= 1.0e-6f)
			{
				return std::unexpected(MakeError(
					AudioSceneRuntimeErrorCode::InvalidComponent,
					"Primary audio listener must have a finite, non-degenerate world transform"));
			}
			return ListenerPose{ Position, Direction / DirectionLength };
		}
	}

	struct AudioSceneRuntime::Impl final
	{
		static constexpr size_t MaxPendingCommands = 256;

		struct Voice final
		{
			Entity BoundEntity;
			AudioPlayback Playback;
			AssetID AudioAsset;
			AudioBus Bus = AudioBus::Effects;
			float Volume = 1.0f;
			bool Looping = false;
			bool Spatialized = false;
			bool EntityPaused = false;
			bool Started = true;
			bool TransitionSuspended = false;
			uint64_t Generation = 0;
		};

		struct PendingCommand final
		{
			uint64_t SessionGeneration = 0;
			uint64_t SceneGeneration = 0;
			Entity Target;
			AudioControlOperation Operation = AudioControlOperation::Play;
			uint64_t ExpectedVoiceGeneration = 0;
			uint64_t NewVoiceGeneration = 0;
			uint64_t Sequence = 0;
		};

		Scene* Source = nullptr;
		std::unordered_map<UUID, Voice, UUIDHash> Voices;
		std::deque<PendingCommand> PendingCommands;
		uint64_t SessionGeneration = 0;
		uint64_t SceneGeneration = 0;
		uint64_t DeviceGeneration = 0;
		uint64_t PreparedDeviceGeneration = 0;
		uint64_t NextVoiceGeneration = 0;
		uint64_t NextCommandSequence = 0;
		bool Paused = false;
		bool ActivationDeferred = false;
		bool Activated = false;
		bool TransitionSuspended = false;

		std::expected<Voice, AudioSceneRuntimeError> CreateVoice(
			AudioEngine& Engine,
			AudioAssetCache& Assets,
			const Entity& EntityValue,
			const AudioSourceComponent& SourceComponent,
			bool ForceStartPaused,
			uint64_t Generation)
		{
			if (auto Validation = SourceComponent.Validate(); !Validation)
				return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::InvalidComponent, Validation.error().Message));

			auto Clip = Assets.GetOrLoad(SourceComponent.AudioAsset);
			if (!Clip)
				return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::AssetLoadFailed, Clip.error().Message));

			AudioPlaybackDesc Description;
			Description.Volume = SourceComponent.Volume;
			Description.Looping = SourceComponent.Looping;
			Description.Spatialized = SourceComponent.Spatialized;
			Description.Bus = SourceComponent.Bus;
			Description.StartPaused = ForceStartPaused || (Paused && SourceComponent.Bus != AudioBus::UI);
			if (SourceComponent.Spatialized)
			{
				const auto World = EntityValue.GetWorldMatrix();
				if (!World)
					return std::unexpected(SceneOperationError(World.error()));
				Description.Position = glm::vec3((*World)[3]);
				if (!IsFinite(Description.Position))
					return std::unexpected(MakeError(
						AudioSceneRuntimeErrorCode::InvalidComponent,
						"Spatial audio source must have a finite world position"));
			}

			auto Playback = Engine.Play(*Clip, Description);
			if (!Playback)
				return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::PlaybackFailed, Playback.error().Message));
			return Voice{
				.BoundEntity = EntityValue,
				.Playback = std::move(*Playback),
				.AudioAsset = SourceComponent.AudioAsset,
				.Bus = SourceComponent.Bus,
				.Volume = SourceComponent.Volume,
				.Looping = SourceComponent.Looping,
				.Spatialized = SourceComponent.Spatialized,
				.EntityPaused = false,
				.Started = !Description.StartPaused,
				.Generation = Generation
			};
		}

		[[nodiscard]] uint64_t GetCurrentVoiceGeneration(const Entity& Target) const noexcept
		{
			const auto Voice = Voices.find(Target.GetUUID());
			return Voice == Voices.end() || Voice->second.BoundEntity != Target ? 0 : Voice->second.Generation;
		}

		void RefreshDeviceGeneration(uint64_t CurrentGeneration)
		{
			if (DeviceGeneration == CurrentGeneration)
				return;
			Voices.clear();
			PendingCommands.clear();
			DeviceGeneration = CurrentGeneration;
		}

		[[nodiscard]] uint64_t GetQueuedVoiceGeneration(const Entity& Target) const noexcept
		{
			for (auto Iterator = PendingCommands.rbegin(); Iterator != PendingCommands.rend(); ++Iterator)
			{
				if (Iterator->Target != Target)
					continue;
				switch (Iterator->Operation)
				{
					case AudioControlOperation::Play:
						return Iterator->NewVoiceGeneration;
					case AudioControlOperation::Stop:
						return 0;
					case AudioControlOperation::Pause:
					case AudioControlOperation::Resume:
						return Iterator->ExpectedVoiceGeneration;
				}
			}
			return GetCurrentVoiceGeneration(Target);
		}

		std::expected<std::optional<ListenerPose>, AudioSceneRuntimeError> FindPrimaryListener(const Scene& SceneValue)
		{
			std::optional<ListenerPose> Pose;
			for (const Entity& EntityValue : SceneValue.GetEntities())
			{
				const auto Listener = EntityValue.GetAudioListener();
				if (!Listener)
					return std::unexpected(SceneOperationError(Listener.error()));
				if (!Listener->has_value() || !Listener->value().IsPrimary)
					continue;
				if (Pose)
					return std::unexpected(MakeError(
						AudioSceneRuntimeErrorCode::MultiplePrimaryListeners,
						"Scene contains more than one primary audio listener"));

				auto CurrentPose = GetListenerPose(EntityValue);
				if (!CurrentPose)
					return std::unexpected(CurrentPose.error());
				Pose = *CurrentPose;
			}
			return Pose;
		}

		std::expected<void, AudioSceneRuntimeError> UpdateListener(AudioEngine& Engine, const Scene& SceneValue)
		{
			auto Pose = FindPrimaryListener(SceneValue);
			if (!Pose)
				return std::unexpected(Pose.error());

			const ListenerPose Current = Pose->value_or(ListenerPose{ glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f) });
			if (auto Result = Engine.SetListenerPosition(Current.Position); !Result)
				return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::InvalidAudioEngine, Result.error().Message));
			if (auto Result = Engine.SetListenerDirection(Current.Direction); !Result)
				return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::InvalidAudioEngine, Result.error().Message));
			return {};
		}
	};

	AudioSceneRuntime::AudioSceneRuntime(
		AudioEngine& Engine,
		AudioAssetCache& Assets,
		uint64_t SessionGeneration,
		uint64_t SceneGeneration)
		: m_Engine(Engine), m_Assets(Assets), m_SessionGeneration(SessionGeneration), m_SceneGeneration(SceneGeneration)
	{
	}

	AudioSceneRuntime::~AudioSceneRuntime() = default;

	std::expected<void, AudioSceneRuntimeError> AudioSceneRuntime::Start(Scene& Source, bool DeferActivation)
	{
		if (m_Impl)
			return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::AlreadyRunning, "Audio scene runtime is already started"));
		if (!m_Engine.IsInitialized())
			return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::InvalidAudioEngine, "Audio engine is not initialized"));

		try
		{
			auto Candidate = std::make_unique<Impl>();
			Candidate->Source = &Source;
			Candidate->SessionGeneration = m_SessionGeneration;
			Candidate->SceneGeneration = m_SceneGeneration;
			Candidate->DeviceGeneration = m_Engine.GetDeviceGeneration();
			Candidate->PreparedDeviceGeneration = Candidate->DeviceGeneration;
			Candidate->ActivationDeferred = DeferActivation;
			Candidate->Activated = !DeferActivation;
			const std::vector<Entity> Entities = Source.GetEntities();
			Candidate->Voices.reserve(Entities.size());
			if (auto Listener = Candidate->FindPrimaryListener(Source); !Listener)
				return std::unexpected(Listener.error());
			for (const Entity& Current : Entities)
			{
				const auto AudioSource = Current.GetAudioSource();
				if (!AudioSource)
					return std::unexpected(SceneOperationError(AudioSource.error()));
				if (!AudioSource->has_value())
					continue;
				if (auto Validation = AudioSource->value().Validate(); !Validation)
					return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::InvalidComponent, Validation.error().Message));
				if (!AudioSource->value().PlayOnStart)
					continue;

				const uint64_t VoiceGeneration = ++Candidate->NextVoiceGeneration;
				auto Voice = Candidate->CreateVoice(
					m_Engine, m_Assets, Current, AudioSource->value(), DeferActivation, VoiceGeneration);
				if (!Voice)
					return std::unexpected(Voice.error());
				Candidate->Voices.emplace(Current.GetUUID(), std::move(*Voice));
			}

			if (!DeferActivation)
			{
				if (auto Listener = Candidate->UpdateListener(m_Engine, Source); !Listener)
					return std::unexpected(Listener.error());
			}
			m_Impl = std::move(Candidate);
			return {};
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(MakeError(
				AudioSceneRuntimeErrorCode::PlaybackFailed,
				std::string("Could not start audio scene runtime: ") + Exception.what()));
		}
	}

	std::expected<void, AudioSceneRuntimeError> AudioSceneRuntime::Activate()
	{
		if (!m_Impl)
			return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::NotRunning, "Audio scene runtime is not started"));
		if (!m_Impl->ActivationDeferred || m_Impl->Activated)
			return {};
		if (m_Impl->PreparedDeviceGeneration != m_Engine.GetDeviceGeneration())
		{
			m_Impl->RefreshDeviceGeneration(m_Engine.GetDeviceGeneration());
			return std::unexpected(MakeError(
				AudioSceneRuntimeErrorCode::InvalidAudioEngine,
				"The audio output changed while the candidate scene was prepared; its staged voices were retired"));
		}

		std::string ActivationFailure;
		for (auto& [Identifier, Voice] : m_Impl->Voices)
		{
			(void)Identifier;
			if (Voice.EntityPaused || (m_Impl->Paused && Voice.Bus != AudioBus::UI))
				continue;
			if (auto Result = Voice.Playback.Resume(); !Result)
			{
				ActivationFailure = Result.error().Message;
				break;
			}
			Voice.Started = true;
		}
		if (!ActivationFailure.empty())
		{
			m_Impl->PendingCommands.clear();
			m_Impl->Voices.clear();
			return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::PlaybackFailed, ActivationFailure));
		}
		if (auto Listener = m_Impl->UpdateListener(m_Engine, *m_Impl->Source); !Listener)
		{
			const AudioSceneRuntimeError Error = Listener.error();
			m_Impl->PendingCommands.clear();
			m_Impl->Voices.clear();
			return std::unexpected(Error);
		}
		m_Impl->Activated = true;
		m_Impl->ActivationDeferred = false;
		return {};
	}

	std::expected<void, AudioSceneRuntimeError> AudioSceneRuntime::SuspendForTransition()
	{
		if (!m_Impl)
			return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::NotRunning, "Audio scene runtime is not started"));
		if (!m_Impl->Activated)
			return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::InvalidCommand, "An unactivated audio candidate cannot be suspended"));
		if (m_Impl->TransitionSuspended)
			return {};

		for (auto& [Identifier, Voice] : m_Impl->Voices)
		{
			(void)Identifier;
			if (Voice.EntityPaused || !Voice.Started)
				continue;
			if (auto Result = Voice.Playback.Pause(); !Result)
			{
				const std::string Message = Result.error().Message;
				std::optional<std::string> RollbackFailure;
				for (auto& [RollbackIdentifier, RollbackVoice] : m_Impl->Voices)
				{
					(void)RollbackIdentifier;
					if (!RollbackVoice.TransitionSuspended)
						continue;
					if (auto ResumeResult = RollbackVoice.Playback.Resume(); ResumeResult)
					{
						RollbackVoice.Started = true;
						RollbackVoice.TransitionSuspended = false;
					}
					else if (!RollbackFailure)
						RollbackFailure = ResumeResult.error().Message;
				}
				if (RollbackFailure)
				{
					m_Impl->TransitionSuspended = true;
					return std::unexpected(MakeError(
						AudioSceneRuntimeErrorCode::PlaybackFailed,
						"Could not suspend all audio voices, and could not restore the retained scene: " + *RollbackFailure));
				}
				return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::PlaybackFailed, Message));
			}
			Voice.Started = false;
			Voice.TransitionSuspended = true;
		}
		m_Impl->TransitionSuspended = true;
		return {};
	}

	std::expected<void, AudioSceneRuntimeError> AudioSceneRuntime::ResumeAfterTransition()
	{
		if (!m_Impl)
			return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::NotRunning, "Audio scene runtime is not started"));
		const bool HasPendingVoiceResume = std::any_of(m_Impl->Voices.begin(), m_Impl->Voices.end(),
			[](const auto& Entry) { return Entry.second.TransitionSuspended; });
		if (!m_Impl->TransitionSuspended && !HasPendingVoiceResume)
			return {};

		if (m_Impl->DeviceGeneration != m_Engine.GetDeviceGeneration())
		{
			m_Impl->RefreshDeviceGeneration(m_Engine.GetDeviceGeneration());
			m_Impl->TransitionSuspended = false;
			return {};
		}
		if (auto Listener = m_Impl->UpdateListener(m_Engine, *m_Impl->Source); !Listener)
			return std::unexpected(Listener.error());

		std::optional<AudioSceneRuntimeError> FirstError;
		for (auto& [Identifier, Voice] : m_Impl->Voices)
		{
			(void)Identifier;
			if (!Voice.TransitionSuspended)
				continue;
			if (Voice.EntityPaused || (m_Impl->Paused && Voice.Bus != AudioBus::UI))
			{
				Voice.TransitionSuspended = false;
				continue;
			}
			const auto CurrentEntity = m_Impl->Source->FindEntity(Voice.BoundEntity.GetUUID());
			if (!CurrentEntity || *CurrentEntity != Voice.BoundEntity)
			{
				Voice.TransitionSuspended = false;
				continue;
			}
			if (auto Result = Voice.Playback.Resume(); !Result)
			{
				if (!FirstError)
					FirstError = MakeError(AudioSceneRuntimeErrorCode::PlaybackFailed, Result.error().Message);
				continue;
			}
			Voice.Started = true;
			Voice.TransitionSuspended = false;
		}
		m_Impl->TransitionSuspended = false;
		if (FirstError)
			return std::unexpected(std::move(*FirstError));
		return {};
	}

	std::expected<void, AudioSceneRuntimeError> AudioSceneRuntime::QueueControl(
		const Entity& Target,
		AudioControlOperation Operation)
	{
		if (!m_Impl)
			return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::NotRunning, "Audio scene runtime is not started"));
		if (m_Impl->SessionGeneration != m_SessionGeneration || m_Impl->SceneGeneration != m_SceneGeneration)
			return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::InvalidCommand, "Audio control request belongs to a stale session or scene generation"));
		if (m_Impl->TransitionSuspended)
			return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::InvalidCommand, "Audio controls are unavailable while this scene is suspended for a transition"));
		if (m_Impl->DeviceGeneration != m_Engine.GetDeviceGeneration())
			m_Impl->RefreshDeviceGeneration(m_Engine.GetDeviceGeneration());
		if (!m_Impl->Activated && !m_Impl->ActivationDeferred)
			return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::InvalidCommand, "Audio controls are unavailable until the candidate scene is activated"));
		if (m_Impl->PendingCommands.size() >= Impl::MaxPendingCommands)
			return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::CommandQueueFull, "Audio control queue is full; request was not accepted"));
		if (m_Impl->NextCommandSequence == std::numeric_limits<uint64_t>::max())
			return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::InvalidCommand, "Audio control sequence space is exhausted"));
		if (!Target.IsValid())
			return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::InvalidCommand, "Audio control target is no longer valid"));

		const UUID Identifier = Target.GetUUID();
		const auto CurrentEntity = m_Impl->Source->FindEntity(Identifier);
		if (!CurrentEntity || *CurrentEntity != Target)
			return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::InvalidCommand, "Audio control target belongs to a different entity incarnation"));

		switch (Operation)
		{
			case AudioControlOperation::Play:
			{
				const auto Component = Target.GetAudioSource();
				if (!Component)
					return std::unexpected(SceneOperationError(Component.error()));
				if (!Component->has_value())
					return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::MissingAudioSource, "Entity does not have an audio source component"));
				if (auto Validation = Component->value().Validate(); !Validation)
					return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::InvalidComponent, Validation.error().Message));
				break;
			}
			case AudioControlOperation::Pause:
			case AudioControlOperation::Resume:
			case AudioControlOperation::Stop:
				break;
			default:
				return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::InvalidCommand, "Audio control operation is not supported"));
		}

		const uint64_t ExpectedVoiceGeneration = m_Impl->GetQueuedVoiceGeneration(Target);
		if (Operation != AudioControlOperation::Play && ExpectedVoiceGeneration == 0)
			return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::MissingPlayback, "Entity has no active or queued audio playback"));
		if (Operation == AudioControlOperation::Play && m_Impl->NextVoiceGeneration == std::numeric_limits<uint64_t>::max())
			return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::InvalidCommand, "Audio voice generation space is exhausted"));

		try
		{
			Impl::PendingCommand Command;
			Command.SessionGeneration = m_Impl->SessionGeneration;
			Command.SceneGeneration = m_Impl->SceneGeneration;
			Command.Target = Target;
			Command.Operation = Operation;
			Command.ExpectedVoiceGeneration = ExpectedVoiceGeneration;
			Command.Sequence = ++m_Impl->NextCommandSequence;
			if (Operation == AudioControlOperation::Play)
				Command.NewVoiceGeneration = ++m_Impl->NextVoiceGeneration;
			m_Impl->PendingCommands.push_back(std::move(Command));
			return {};
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(MakeError(
				AudioSceneRuntimeErrorCode::InvalidCommand,
				std::string("Could not queue audio control request: ") + Exception.what()));
		}
	}

	std::expected<void, AudioSceneRuntimeError> AudioSceneRuntime::ProcessPendingCommands()
	{
		if (!m_Impl)
			return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::NotRunning, "Audio scene runtime is not started"));
		if (m_Impl->SessionGeneration != m_SessionGeneration || m_Impl->SceneGeneration != m_SceneGeneration)
		{
			m_Impl->PendingCommands.clear();
			return {};
		}
		if (!m_Impl->Activated)
			return {};
		if (m_Impl->TransitionSuspended)
			return {};
		if (m_Impl->DeviceGeneration != m_Engine.GetDeviceGeneration())
		{
			m_Impl->RefreshDeviceGeneration(m_Engine.GetDeviceGeneration());
			return {};
		}

		std::optional<AudioSceneRuntimeError> FirstError;
		while (!m_Impl->PendingCommands.empty())
		{
			Impl::PendingCommand Command = std::move(m_Impl->PendingCommands.front());
			m_Impl->PendingCommands.pop_front();
			if (Command.SessionGeneration != m_Impl->SessionGeneration ||
				Command.SceneGeneration != m_Impl->SceneGeneration ||
				!Command.Target.IsValid())
				continue;

			const UUID Identifier = Command.Target.GetUUID();
			const auto CurrentEntity = m_Impl->Source->FindEntity(Identifier);
			if (!CurrentEntity || *CurrentEntity != Command.Target ||
				m_Impl->GetCurrentVoiceGeneration(Command.Target) != Command.ExpectedVoiceGeneration)
				continue;

			std::expected<void, AudioSceneRuntimeError> Result;
			try
			{
				switch (Command.Operation)
				{
					case AudioControlOperation::Play:
					{
						const auto Component = Command.Target.GetAudioSource();
						if (!Component)
						{
							Result = std::unexpected(SceneOperationError(Component.error()));
							break;
						}
						if (!Component->has_value())
						{
							Result = std::unexpected(MakeError(
								AudioSceneRuntimeErrorCode::MissingAudioSource,
								"Audio source was removed before its queued play request was applied"));
							break;
						}
						auto NewVoice = m_Impl->CreateVoice(
							m_Engine,
							m_Assets,
							Command.Target,
							Component->value(),
							false,
							Command.NewVoiceGeneration);
						if (!NewVoice)
							Result = std::unexpected(NewVoice.error());
						else
							m_Impl->Voices.insert_or_assign(Identifier, std::move(*NewVoice));
						break;
					}
					case AudioControlOperation::Pause:
						Result = Pause(Identifier);
						break;
					case AudioControlOperation::Resume:
						Result = Resume(Identifier);
						break;
					case AudioControlOperation::Stop:
						Result = StopPlayback(Identifier);
						break;
				}
			}
			catch (const std::exception& Exception)
			{
				Result = std::unexpected(MakeError(
					AudioSceneRuntimeErrorCode::PlaybackFailed,
					std::string("Could not apply queued audio control: ") + Exception.what()));
			}
			if (!Result && !FirstError)
				FirstError = Result.error();
		}
		if (FirstError)
			return std::unexpected(std::move(*FirstError));
		return {};
	}

	void AudioSceneRuntime::Stop() noexcept
	{
		m_Impl.reset();
	}

	bool AudioSceneRuntime::IsRunning() const noexcept
	{
		return m_Impl != nullptr;
	}

	size_t AudioSceneRuntime::GetPlaybackCount() const noexcept
	{
		return m_Impl ? m_Impl->Voices.size() : 0;
	}

	std::expected<bool, AudioSceneRuntimeError> AudioSceneRuntime::IsPlaybackPlaying(UUID Identifier) const
	{
		if (!m_Impl)
			return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::NotRunning, "Audio scene runtime is not started"));
		if (m_Impl->TransitionSuspended)
			return false;
		if (m_Impl->DeviceGeneration != m_Engine.GetDeviceGeneration())
			return false;
		const auto Voice = m_Impl->Voices.find(Identifier);
		if (Voice == m_Impl->Voices.end())
			return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::MissingPlayback, "Entity has no active audio playback"));
		const auto EntityValue = m_Impl->Source->FindEntity(Identifier);
		if (!EntityValue || *EntityValue != Voice->second.BoundEntity)
			return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::MissingPlayback, "Entity has no active audio playback"));
		return Voice->second.Playback.IsPlaying();
	}

	std::expected<void, AudioSceneRuntimeError> AudioSceneRuntime::SetPaused(bool Paused)
	{
		if (!m_Impl) return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::NotRunning, "Audio scene runtime is not started"));
		if (m_Impl->TransitionSuspended)
			return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::InvalidCommand, "Pause state cannot change while audio is suspended for a transition"));
		if (m_Impl->DeviceGeneration != m_Engine.GetDeviceGeneration())
			m_Impl->RefreshDeviceGeneration(m_Engine.GetDeviceGeneration());
		if (m_Impl->Paused == Paused) return {};

		std::vector<Impl::Voice*> ChangedVoices;
		for (auto& [Identifier, Voice] : m_Impl->Voices)
		{
			(void)Identifier;
			if (Voice.Bus == AudioBus::UI)
				continue;
			if (Paused)
			{
				if (Voice.EntityPaused || !Voice.Started) continue;
				if (auto Result = Voice.Playback.Pause(); !Result)
				{
					for (Impl::Voice* Changed : ChangedVoices)
					{
						(void)Changed->Playback.Resume();
						Changed->Started = true;
					}
					return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::PlaybackFailed, Result.error().Message));
				}
				Voice.Started = false;
				ChangedVoices.push_back(&Voice);
			}
			else if (!Voice.EntityPaused && m_Impl->Activated && !Voice.Started)
			{
				if (auto Result = Voice.Playback.Resume(); !Result)
				{
					for (Impl::Voice* Changed : ChangedVoices)
					{
						(void)Changed->Playback.Pause();
						Changed->Started = false;
					}
					return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::PlaybackFailed, Result.error().Message));
				}
				Voice.Started = true;
				Voice.TransitionSuspended = false;
				ChangedVoices.push_back(&Voice);
			}
		}
		m_Impl->Paused = Paused;
		return {};
	}

	std::expected<void, AudioSceneRuntimeError> AudioSceneRuntime::Advance(Scene& Source)
	{
		if (!m_Impl)
			return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::NotRunning, "Audio scene runtime is not started"));
		if (m_Impl->Source != &Source)
			return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::DifferentScene, "Audio runtime was started for a different Scene"));
		if (m_Impl->DeviceGeneration != m_Engine.GetDeviceGeneration())
			m_Impl->RefreshDeviceGeneration(m_Engine.GetDeviceGeneration());
		if (!m_Impl->Activated || m_Impl->TransitionSuspended)
			return {};

		if (auto Listener = m_Impl->UpdateListener(m_Engine, Source); !Listener)
			return std::unexpected(Listener.error());

		for (auto Iterator = m_Impl->Voices.begin(); Iterator != m_Impl->Voices.end();)
		{
			const auto EntityValue = Source.FindEntity(Iterator->first);
			if (!EntityValue || *EntityValue != Iterator->second.BoundEntity)
			{
				Iterator = m_Impl->Voices.erase(Iterator);
				continue;
			}

			const auto AudioSource = EntityValue->GetAudioSource();
			if (!AudioSource)
				return std::unexpected(SceneOperationError(AudioSource.error()));
			if (!AudioSource->has_value() || Iterator->second.Playback.IsAtEnd())
			{
				Iterator = m_Impl->Voices.erase(Iterator);
				continue;
			}
			const AudioSourceComponent& Component = AudioSource->value();
			if (auto Validation = Component.Validate(); !Validation)
				return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::InvalidComponent, Validation.error().Message));
			if (Component.AudioAsset != Iterator->second.AudioAsset || Component.Bus != Iterator->second.Bus)
			{
				const bool PreserveEntityPause = Iterator->second.EntityPaused;
				const uint64_t VoiceGeneration = ++m_Impl->NextVoiceGeneration;
				auto Replacement = m_Impl->CreateVoice(
					m_Engine,
					m_Assets,
					*EntityValue,
					Component,
					PreserveEntityPause,
					VoiceGeneration);
				if (!Replacement)
					return std::unexpected(Replacement.error());
				Replacement->EntityPaused = PreserveEntityPause;
				Iterator->second = std::move(*Replacement);
				++Iterator;
				continue;
			}
			if (Component.Volume != Iterator->second.Volume)
			{
				if (auto Update = Iterator->second.Playback.SetVolume(Component.Volume); !Update)
					return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::PlaybackFailed, Update.error().Message));
				Iterator->second.Volume = Component.Volume;
			}
			if (Component.Looping != Iterator->second.Looping)
			{
				if (auto Update = Iterator->second.Playback.SetLooping(Component.Looping); !Update)
					return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::PlaybackFailed, Update.error().Message));
				Iterator->second.Looping = Component.Looping;
			}
			if (Component.Spatialized != Iterator->second.Spatialized)
			{
				if (auto Update = Iterator->second.Playback.SetSpatialized(Component.Spatialized); !Update)
					return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::PlaybackFailed, Update.error().Message));
				Iterator->second.Spatialized = Component.Spatialized;
			}

			if (Iterator->second.Spatialized)
			{
				const auto World = EntityValue->GetWorldMatrix();
				if (!World)
					return std::unexpected(SceneOperationError(World.error()));
				const glm::vec3 Position((*World)[3]);
				if (!IsFinite(Position))
					return std::unexpected(MakeError(
						AudioSceneRuntimeErrorCode::InvalidComponent,
						"Spatial audio source must have a finite world position"));
				if (auto Update = Iterator->second.Playback.SetPosition(Position); !Update)
					return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::PlaybackFailed, Update.error().Message));
			}
			++Iterator;
		}
		return {};
	}

	std::expected<void, AudioSceneRuntimeError> AudioSceneRuntime::Play(UUID Identifier)
	{
		if (!m_Impl)
			return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::NotRunning, "Audio scene runtime is not started"));
		if (m_Impl->TransitionSuspended)
			return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::InvalidCommand, "Audio controls are unavailable while this scene is suspended for a transition"));
		if (m_Impl->DeviceGeneration != m_Engine.GetDeviceGeneration())
			m_Impl->RefreshDeviceGeneration(m_Engine.GetDeviceGeneration());
		if (!m_Impl->Activated)
			return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::InvalidCommand, "Audio controls are unavailable until the candidate scene is activated"));
		const auto EntityValue = m_Impl->Source->FindEntity(Identifier);
		if (!EntityValue)
			return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::MissingAudioSource, "Audio source entity does not exist"));
		const auto AudioSource = EntityValue->GetAudioSource();
		if (!AudioSource)
			return std::unexpected(SceneOperationError(AudioSource.error()));
		if (!AudioSource->has_value())
			return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::MissingAudioSource, "Entity does not have an audio source component"));
		if (m_Impl->NextVoiceGeneration == std::numeric_limits<uint64_t>::max())
			return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::InvalidCommand, "Audio voice generation space is exhausted"));

		try
		{
			const uint64_t VoiceGeneration = ++m_Impl->NextVoiceGeneration;
			auto Voice = m_Impl->CreateVoice(
				m_Engine, m_Assets, *EntityValue, AudioSource->value(), false, VoiceGeneration);
			if (!Voice)
				return std::unexpected(Voice.error());
			m_Impl->Voices.insert_or_assign(Identifier, std::move(*Voice));
			return {};
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(MakeError(
				AudioSceneRuntimeErrorCode::PlaybackFailed,
				std::string("Could not create audio playback: ") + Exception.what()));
		}
	}

	std::expected<void, AudioSceneRuntimeError> AudioSceneRuntime::Pause(UUID Identifier)
	{
		if (!m_Impl)
			return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::NotRunning, "Audio scene runtime is not started"));
		if (m_Impl->TransitionSuspended)
			return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::InvalidCommand, "Audio controls are unavailable while this scene is suspended for a transition"));
		if (m_Impl->DeviceGeneration != m_Engine.GetDeviceGeneration())
			m_Impl->RefreshDeviceGeneration(m_Engine.GetDeviceGeneration());
		if (!m_Impl->Activated)
			return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::InvalidCommand, "Audio controls are unavailable until the candidate scene is activated"));
		const auto Voice = m_Impl->Voices.find(Identifier);
		if (Voice == m_Impl->Voices.end())
			return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::MissingPlayback, "Entity has no active audio playback"));
		const auto EntityValue = m_Impl->Source->FindEntity(Identifier);
		if (!EntityValue || *EntityValue != Voice->second.BoundEntity)
		{
			m_Impl->Voices.erase(Voice);
			return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::MissingPlayback, "Entity has no active audio playback"));
		}
		if (Voice->second.Started)
		{
			if (auto Result = Voice->second.Playback.Pause(); !Result)
				return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::PlaybackFailed, Result.error().Message));
			Voice->second.Started = false;
		}
		Voice->second.EntityPaused = true;
		Voice->second.TransitionSuspended = false;
		return {};
	}

	std::expected<void, AudioSceneRuntimeError> AudioSceneRuntime::Resume(UUID Identifier)
	{
		if (!m_Impl)
			return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::NotRunning, "Audio scene runtime is not started"));
		if (m_Impl->TransitionSuspended)
			return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::InvalidCommand, "Audio controls are unavailable while this scene is suspended for a transition"));
		if (m_Impl->DeviceGeneration != m_Engine.GetDeviceGeneration())
			m_Impl->RefreshDeviceGeneration(m_Engine.GetDeviceGeneration());
		if (!m_Impl->Activated)
			return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::InvalidCommand, "Audio controls are unavailable until the candidate scene is activated"));
		const auto Voice = m_Impl->Voices.find(Identifier);
		if (Voice == m_Impl->Voices.end())
			return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::MissingPlayback, "Entity has no active audio playback"));
		const auto EntityValue = m_Impl->Source->FindEntity(Identifier);
		if (!EntityValue || *EntityValue != Voice->second.BoundEntity)
		{
			m_Impl->Voices.erase(Voice);
			return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::MissingPlayback, "Entity has no active audio playback"));
		}
		if (!m_Impl->TransitionSuspended && (!m_Impl->Paused || Voice->second.Bus == AudioBus::UI))
		{
			if (!Voice->second.Started)
			{
				if (auto Result = Voice->second.Playback.Resume(); !Result)
					return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::PlaybackFailed, Result.error().Message));
				Voice->second.Started = true;
			}
		}
		Voice->second.TransitionSuspended = false;
		Voice->second.EntityPaused = false;
		return {};
	}

	std::expected<void, AudioSceneRuntimeError> AudioSceneRuntime::StopPlayback(UUID Identifier)
	{
		if (!m_Impl)
			return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::NotRunning, "Audio scene runtime is not started"));
		if (m_Impl->TransitionSuspended)
			return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::InvalidCommand, "Audio controls are unavailable while this scene is suspended for a transition"));
		if (m_Impl->DeviceGeneration != m_Engine.GetDeviceGeneration())
			m_Impl->RefreshDeviceGeneration(m_Engine.GetDeviceGeneration());
		if (!m_Impl->Activated)
			return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::InvalidCommand, "Audio controls are unavailable until the candidate scene is activated"));
		const auto Voice = m_Impl->Voices.find(Identifier);
		if (Voice == m_Impl->Voices.end())
			return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::MissingPlayback, "Entity has no active audio playback"));
		const auto EntityValue = m_Impl->Source->FindEntity(Identifier);
		if (!EntityValue || *EntityValue != Voice->second.BoundEntity)
		{
			m_Impl->Voices.erase(Voice);
			return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::MissingPlayback, "Entity has no active audio playback"));
		}
		m_Impl->Voices.erase(Voice);
		return {};
	}
}
