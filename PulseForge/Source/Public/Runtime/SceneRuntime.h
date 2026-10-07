#pragma once

#include "Audio/AudioEngine.h"
#include "Core/Core.h"
#include "Core/Timestep.h"
#include "Physics/PhysicsSceneRuntime.h"
#include "Scripting/ScriptRuntime.h"

#include <expected>
#include <memory>
#include <string>

namespace PulseForge
{
	class Input;
	class Project;
	class Scene;

	struct SceneRuntimeDesc
	{
		AudioEngineDesc Audio{};
		PhysicsSceneRuntimeDesc Physics{};
		ScriptRuntimeDesc Scripting{};
	};

	// Non-owning host services supplied by the application to the scene runtime.
	struct SceneRuntimeServices
	{
		Input* InputState = nullptr;
	};

	enum class SceneRuntimeErrorCode : uint8_t
	{
		AlreadyRunning,
		NotRunning,
		DifferentScene,
		InvalidDeltaTime,
		InitializationFailed,
		UpdateFailed
	};

	enum class SceneRuntimeSubsystem : uint8_t
	{
		None,
		AudioDevice,
		Scripting,
		Physics,
		Audio
	};

	struct SceneRuntimeError
	{
		SceneRuntimeErrorCode Code;
		SceneRuntimeSubsystem Subsystem;
		std::string Message;
	};

	struct SceneRuntimeFrameResult
	{
		uint32_t PhysicsSteps = 0;
	};

	// Project, optional host services, and started Scene must outlive this coordinator; keep the scene at the same address while running.
	class PULSEFORGE_API SceneRuntime final
	{
	public:
		explicit SceneRuntime(
			const Project& SourceProject,
			SceneRuntimeDesc Description = {},
			SceneRuntimeServices Services = {});
		~SceneRuntime();
		SceneRuntime(const SceneRuntime&) = delete;
		SceneRuntime& operator=(const SceneRuntime&) = delete;

		[[nodiscard]] std::expected<void, SceneRuntimeError> Start(Scene& Source);
		void Stop() noexcept;
		[[nodiscard]] bool IsRunning() const noexcept;
		// Updates scripts, advances fixed-step physics, then synchronizes spatial audio before rendering.
		[[nodiscard]] std::expected<SceneRuntimeFrameResult, SceneRuntimeError> Advance(
			Scene& Source,
			Timestep FrameDelta);

	private:
		struct Impl;
		const Project& m_Project;
		SceneRuntimeDesc m_Description;
		SceneRuntimeServices m_Services;
		std::unique_ptr<Impl> m_Impl;
	};
}
