#include "Core/PulseForgePCH.h"
#include "Audio/AudioSceneRuntime.h"

#include "Assets/AudioAssetCache.h"
#include "Audio/AudioEngine.h"
#include "Scene/Components/AudioListenerComponent.h"
#include "Scene/Components/AudioSourceComponent.h"
#include "Scene/Entity.h"
#include "Scene/Scene.h"

#include <cmath>
#include <unordered_map>
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
		struct Voice final
		{
			AudioPlayback Playback;
			AssetID AudioAsset;
			float Volume = 1.0f;
			bool Looping = false;
			bool Spatialized = false;
		};

		Scene* Source = nullptr;
		std::unordered_map<UUID, Voice, UUIDHash> Voices;

		std::expected<Voice, AudioSceneRuntimeError> CreateVoice(
			AudioEngine& Engine,
			AudioAssetCache& Assets,
			const Entity& EntityValue,
			const AudioSourceComponent& SourceComponent)
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
				std::move(*Playback),
				SourceComponent.AudioAsset,
				SourceComponent.Volume,
				SourceComponent.Looping,
				SourceComponent.Spatialized };
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

	AudioSceneRuntime::AudioSceneRuntime(AudioEngine& Engine, AudioAssetCache& Assets)
		: m_Engine(Engine), m_Assets(Assets)
	{
	}

	AudioSceneRuntime::~AudioSceneRuntime() = default;

	std::expected<void, AudioSceneRuntimeError> AudioSceneRuntime::Start(Scene& Source)
	{
		if (m_Impl)
			return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::AlreadyRunning, "Audio scene runtime is already started"));
		if (!m_Engine.IsInitialized())
			return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::InvalidAudioEngine, "Audio engine is not initialized"));

		try
		{
			auto Candidate = std::make_unique<Impl>();
			Candidate->Source = &Source;
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

				auto Voice = Candidate->CreateVoice(m_Engine, m_Assets, Current, AudioSource->value());
				if (!Voice)
					return std::unexpected(Voice.error());
				Candidate->Voices.emplace(Current.GetUUID(), std::move(*Voice));
			}

			if (auto Listener = Candidate->UpdateListener(m_Engine, Source); !Listener)
				return std::unexpected(Listener.error());
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

	std::expected<void, AudioSceneRuntimeError> AudioSceneRuntime::Advance(Scene& Source)
	{
		if (!m_Impl)
			return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::NotRunning, "Audio scene runtime is not started"));
		if (m_Impl->Source != &Source)
			return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::DifferentScene, "Audio runtime was started for a different Scene"));

		if (auto Listener = m_Impl->UpdateListener(m_Engine, Source); !Listener)
			return std::unexpected(Listener.error());

		for (auto Iterator = m_Impl->Voices.begin(); Iterator != m_Impl->Voices.end();)
		{
			const auto EntityValue = Source.FindEntity(Iterator->first);
			if (!EntityValue)
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
			if (Component.AudioAsset != Iterator->second.AudioAsset)
			{
				auto Replacement = m_Impl->CreateVoice(m_Engine, m_Assets, *EntityValue, Component);
				if (!Replacement)
					return std::unexpected(Replacement.error());
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
		const auto EntityValue = m_Impl->Source->FindEntity(Identifier);
		if (!EntityValue)
			return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::MissingAudioSource, "Audio source entity does not exist"));
		const auto AudioSource = EntityValue->GetAudioSource();
		if (!AudioSource)
			return std::unexpected(SceneOperationError(AudioSource.error()));
		if (!AudioSource->has_value())
			return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::MissingAudioSource, "Entity does not have an audio source component"));

		try
		{
			auto Voice = m_Impl->CreateVoice(m_Engine, m_Assets, *EntityValue, AudioSource->value());
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
		const auto Voice = m_Impl->Voices.find(Identifier);
		if (Voice == m_Impl->Voices.end())
			return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::MissingPlayback, "Entity has no active audio playback"));
		if (auto Result = Voice->second.Playback.Pause(); !Result)
			return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::PlaybackFailed, Result.error().Message));
		return {};
	}

	std::expected<void, AudioSceneRuntimeError> AudioSceneRuntime::Resume(UUID Identifier)
	{
		if (!m_Impl)
			return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::NotRunning, "Audio scene runtime is not started"));
		const auto Voice = m_Impl->Voices.find(Identifier);
		if (Voice == m_Impl->Voices.end())
			return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::MissingPlayback, "Entity has no active audio playback"));
		if (auto Result = Voice->second.Playback.Resume(); !Result)
			return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::PlaybackFailed, Result.error().Message));
		return {};
	}

	std::expected<void, AudioSceneRuntimeError> AudioSceneRuntime::StopPlayback(UUID Identifier)
	{
		if (!m_Impl)
			return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::NotRunning, "Audio scene runtime is not started"));
		const auto Voice = m_Impl->Voices.find(Identifier);
		if (Voice == m_Impl->Voices.end())
			return std::unexpected(MakeError(AudioSceneRuntimeErrorCode::MissingPlayback, "Entity has no active audio playback"));
		m_Impl->Voices.erase(Voice);
		return {};
	}
}
