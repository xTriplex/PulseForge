#include "Core/PulseForgePCH.h"
#include "Runtime/SceneRuntime.h"

#include "Assets/AudioAssetCache.h"
#include "Assets/Project.h"
#include "Audio/AudioSceneRuntime.h"
#include "Physics/PhysicsSceneRuntime.h"
#include "Scene/Scene.h"
#include "Scripting/ScriptRuntime.h"

#include <optional>

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
	}

	struct SceneRuntime::Impl final
	{
		// Declaration order intentionally makes script callbacks stop before their services and dependencies.
		std::optional<AudioEngine> AudioDevice;
		std::unique_ptr<AudioAssetCache> AudioAssets;
		std::unique_ptr<AudioSceneRuntime> Audio;
		std::unique_ptr<PhysicsSceneRuntime> Physics;
		std::unique_ptr<ScriptRuntime> Scripting;
		Scene* Source = nullptr;
	};

	SceneRuntime::SceneRuntime(const Project& SourceProject, SceneRuntimeDesc Description)
		: m_Project(SourceProject), m_Description(Description)
	{
	}

	SceneRuntime::~SceneRuntime() = default;

	std::expected<void, SceneRuntimeError> SceneRuntime::Start(Scene& Source)
	{
		if (m_Impl)
			return std::unexpected(MakeError(
				SceneRuntimeErrorCode::AlreadyRunning,
				SceneRuntimeSubsystem::None,
				"Scene runtime is already started"));

		try
		{
			auto Candidate = std::make_unique<Impl>();
			Candidate->Source = &Source;

			auto AudioDevice = AudioEngine::Create(m_Description.Audio);
			if (!AudioDevice)
				return std::unexpected(MakeError(
					SceneRuntimeErrorCode::InitializationFailed,
					SceneRuntimeSubsystem::AudioDevice,
					"Could not initialize the audio device: " + AudioDevice.error().Message));
			Candidate->AudioDevice.emplace(std::move(*AudioDevice));
			Candidate->AudioAssets = std::make_unique<AudioAssetCache>(
				*Candidate->AudioDevice,
				m_Project.GetRootPath(),
				m_Project.GetAssetRegistry());

			Candidate->Physics = std::make_unique<PhysicsSceneRuntime>(m_Description.Physics);
			if (auto Result = Candidate->Physics->Start(Source); !Result)
				return std::unexpected(MakeError(
					SceneRuntimeErrorCode::InitializationFailed,
					SceneRuntimeSubsystem::Physics,
					"Could not start scene physics: " + Result.error().Message));

			Candidate->Audio = std::make_unique<AudioSceneRuntime>(*Candidate->AudioDevice, *Candidate->AudioAssets);
			if (auto Result = Candidate->Audio->Start(Source); !Result)
				return std::unexpected(MakeError(
					SceneRuntimeErrorCode::InitializationFailed,
					SceneRuntimeSubsystem::Audio,
					"Could not start scene audio: " + Result.error().Message));

			const ScriptRuntimeServices Services{
				.Physics = Candidate->Physics.get(),
				.Audio = Candidate->Audio.get() };
			Candidate->Scripting = std::make_unique<ScriptRuntime>(m_Project, m_Description.Scripting, Services);
			if (auto Result = Candidate->Scripting->Start(Source); !Result)
				return std::unexpected(MakeError(
					SceneRuntimeErrorCode::InitializationFailed,
					SceneRuntimeSubsystem::Scripting,
					"Could not start scene scripting: " + Result.error().Message));

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

	void SceneRuntime::Stop() noexcept
	{
		m_Impl.reset();
	}

	bool SceneRuntime::IsRunning() const noexcept
	{
		return m_Impl != nullptr;
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

		if (auto Result = m_Impl->Scripting->Advance(Source, FrameDelta); !Result)
			return std::unexpected(MakeError(
				SceneRuntimeErrorCode::UpdateFailed,
				SceneRuntimeSubsystem::Scripting,
				"Could not update scene scripts: " + Result.error().Message));

		auto PhysicsResult = m_Impl->Physics->Advance(Source, FrameDelta);
		if (!PhysicsResult)
			return std::unexpected(MakeError(
				SceneRuntimeErrorCode::UpdateFailed,
				SceneRuntimeSubsystem::Physics,
				"Could not advance scene physics: " + PhysicsResult.error().Message));

		if (auto Result = m_Impl->Audio->Advance(Source); !Result)
			return std::unexpected(MakeError(
				SceneRuntimeErrorCode::UpdateFailed,
				SceneRuntimeSubsystem::Audio,
				"Could not update scene audio: " + Result.error().Message));

		return SceneRuntimeFrameResult{ *PhysicsResult };
	}
}
