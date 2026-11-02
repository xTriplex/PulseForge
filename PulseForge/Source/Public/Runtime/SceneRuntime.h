#pragma once

#include "Audio/AudioEngine.h"
#include "Core/Core.h"
#include "Core/Timestep.h"
#include "Input/GameplayInput.h"
#include "Physics/PhysicsSceneRuntime.h"
#include "Scripting/ScriptRuntime.h"
#include "Scene/Components/RuntimeUICanvasComponent.h"
#include "UI/RuntimeUI.h"

#include "Assets/AssetID.h"

#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <memory>
#include <string>

namespace PulseForge
{
	class Input;
	class DiagnosticRecorder;
	class Project;
	class Scene;
	class CookedContentProvider;

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
		GameplayInputService* GameplayInput = nullptr;
		DiagnosticRecorder* Diagnostics = nullptr;
		// Optional host policy check; an error rejects a pause change before audio state is modified.
		std::function<std::optional<std::string>(bool)> AudioPauseGuard;
		ScriptRuntimeObserver ScriptObserver;
		// Accepted requests are committed by the owning host after SceneRuntime::Advance returns.
		std::function<bool(const AssetID&)> QueueSceneTransition;
		std::function<bool()> QueueStop;
	};

	enum class SceneRuntimeBoundaryRequestKind : uint8_t
	{
		SceneTransition,
		Stop
	};

	struct SceneRuntimeBoundaryRequest
	{
		uint64_t SessionGeneration = 0;
		uint64_t RequestGeneration = 0;
		SceneRuntimeBoundaryRequestKind Kind = SceneRuntimeBoundaryRequestKind::SceneTransition;
		std::optional<AssetID> Scene;
	};

	enum class SceneRuntimeBoundaryResolution : uint8_t
	{
		Committed,
		Rejected,
		Cancelled
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
		PhysicsSceneRuntimeStats PhysicsStats{};
	};

	// Project, optional host services, and started Scene must outlive this coordinator; keep the scene at the same address while running.
	class PULSEFORGE_API SceneRuntime final
	{
	public:
#if !defined(PF_COOKED_RUNTIME_ONLY)
		explicit SceneRuntime(
			const Project& SourceProject,
			SceneRuntimeDesc Description = {},
			SceneRuntimeServices Services = {});
#endif
		explicit SceneRuntime(
			std::shared_ptr<const CookedContentProvider> CookedAssets,
			SceneRuntimeDesc Description = {},
			SceneRuntimeServices Services = {});
		~SceneRuntime();
		SceneRuntime(const SceneRuntime&) = delete;
		SceneRuntime& operator=(const SceneRuntime&) = delete;

		[[nodiscard]] std::expected<void, SceneRuntimeError> Start(Scene& Source);
		void Stop() noexcept;
		[[nodiscard]] bool IsRunning() const noexcept;
		[[nodiscard]] std::expected<void, SceneRuntimeError> SetPaused(bool Paused);
		[[nodiscard]] bool IsPaused() const noexcept;
		[[nodiscard]] bool HasRuntimeUI() const noexcept;
		[[nodiscard]] RuntimeUIInputPolicy GetRuntimeUIInputPolicy() const noexcept;
		[[nodiscard]] uint64_t GetRuntimeUIInputGeneration() const noexcept;
		// Owner/window code supplies viewport geometry and a bounded logical input batch before Advance.
		// Submit once per host frame while a canvas exists, including passive frames with no input events.
		[[nodiscard]] bool SubmitRuntimeUIFrame(RuntimeUIViewport Viewport, std::span<const RuntimeUIInputEvent> Events);
		[[nodiscard]] std::span<const RuntimeUIDrawCommand> GetRuntimeUIDrawCommands() const noexcept;
		[[nodiscard]] RuntimeUIViewport GetRuntimeUIViewport() const noexcept;
		// Queues an explicit state-reset reload for one attachment in the active Play/Player scene.
		[[nodiscard]] std::expected<uint64_t, SceneRuntimeError> RequestScriptReload(const Entity& Target);
		[[nodiscard]] std::span<const ScriptDiagnostic> GetScriptDiagnostics() const noexcept;
		// Owners inspect requests only after Advance returns, then acknowledge the matching session/request token.
		[[nodiscard]] std::optional<SceneRuntimeBoundaryRequest> GetPendingBoundaryRequest() const noexcept;
		[[nodiscard]] bool ResolveBoundaryRequest(
			const SceneRuntimeBoundaryRequest& Request,
			SceneRuntimeBoundaryResolution Resolution) noexcept;
		// Updates scripts, advances fixed-step physics, then synchronizes spatial audio before rendering.
		[[nodiscard]] std::expected<SceneRuntimeFrameResult, SceneRuntimeError> Advance(
			Scene& Source,
			Timestep FrameDelta);

	private:
		struct Impl;
#if !defined(PF_COOKED_RUNTIME_ONLY)
		const Project* m_Project = nullptr;
#endif
		std::shared_ptr<const CookedContentProvider> m_CookedAssets;
		SceneRuntimeDesc m_Description;
		SceneRuntimeServices m_Services;
		std::unique_ptr<Impl> m_Impl;
	};
}
