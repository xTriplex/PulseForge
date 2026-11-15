#pragma once

#include "Audio/AudioEngine.h"
#include "Core/Core.h"
#include "Core/Timestep.h"
#include "Input/GameplayInput.h"
#include "Physics/PhysicsSceneRuntime.h"
#include "Scripting/ScriptRuntime.h"
#include "Scripting/BlueprintProgram.h"
#include "Scene/Components/RuntimeUICanvasComponent.h"
#include "Scene/Entity.h"
#include "UI/RuntimeUI.h"

#include "Assets/AssetID.h"

#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <memory>
#include <string>
#include <span>

namespace PulseForge
{
	class Entity;
	class Input;
	class DiagnosticRecorder;
	class Project;
	class Scene;
	class CookedContentProvider;
	class AudioSession;

	struct SceneRuntimeDesc
	{
		AudioEngineDesc Audio{};
		PhysicsSceneRuntimeDesc Physics{};
		ScriptRuntimeDesc Scripting{};
	};

	struct BlueprintProgramBinding
	{
		AssetID Asset;
		uint64_t Revision = 0;
		std::shared_ptr<const VerifiedBlueprintProgram> Program;
	};

	struct BlueprintRuntimeDiagnostic
	{
		UUID Entity;
		AssetID Asset;
		BlueprintEventKind Event = BlueprintEventKind::OnCreate;
		BlueprintProgramErrorCode Code = BlueprintProgramErrorCode::InvalidControlFlow;
		UUID SourceNode;
		std::string Message;
	};

	// An in-memory debugger token for one entity incarnation and immutable Blueprint revision.
	// The weak Entity handle never keeps the runtime target alive and becomes invalid on destruction.
	struct BlueprintRuntimeTraceHandle
	{
		Entity Target;
		AssetID Asset;
		uint64_t SessionGeneration = 0;
		uint64_t ProgramGeneration = 0;
		uint64_t Revision = 0;
	};

	struct BlueprintRuntimeTrace
	{
		BlueprintRuntimeTraceHandle Handle;
		BlueprintEventKind Event = BlueprintEventKind::OnCreate;
		std::vector<UUID> ExecutedNodes;
		bool ExecutedNodesTruncated = false;
		std::vector<std::string> ScalarPropertyIds;
		std::vector<float> ScalarValues;
	};

	inline constexpr size_t BlueprintMaximumRuntimeTraces = 128;

	// Non-owning host services supplied by the application to the scene runtime.
	struct SceneRuntimeServices
	{
		Input* InputState = nullptr;
		GameplayInputService* GameplayInput = nullptr;
		DiagnosticRecorder* Diagnostics = nullptr;
		// Optional session owner shared across scene replacements. When null, SceneRuntime creates an owned session.
		AudioSession* AudioSessionState = nullptr;
		// Optional host policy check; an error rejects a pause change before audio state is modified.
		std::function<std::optional<std::string>(bool)> AudioPauseGuard;
		ScriptRuntimeObserver ScriptObserver;
		// The host resolves immutable, already-verified event programs for the active scene session.
		std::function<std::optional<BlueprintProgramBinding>(const Entity&, BlueprintEventKind)> ResolveBlueprintProgram;
		// Changes invalidate per-entity Blueprint state and force the resolver to bind the new immutable revision.
		std::function<uint64_t()> GetBlueprintProgramGeneration;
		// Reads authored initial scalar values once when an asset instance is first bound.
		std::function<std::optional<float>(const Entity&, std::string_view)> ReadBlueprintScalarProperty;
		// Applies one event's scalar property writes as a single owner-thread transaction.
		std::function<bool(const Entity&, std::span<const BlueprintScalarPropertyWrite>)> ApplyBlueprintPropertyWrites;
		// Accepted requests are committed by the owning host after SceneRuntime::Advance returns.
		std::function<bool(const AssetID&)> QueueSceneTransition;
		std::function<bool()> QueueStop;
		std::function<bool(bool)> QueuePause;
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
		uint32_t BlueprintInstructionsExecuted = 0;
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

		[[nodiscard]] std::expected<void, SceneRuntimeError> Start(Scene& Source, bool DeferAudioActivation = false);
		// Starts candidate PlayOnStart voices after the host commits a prepared scene transition.
		[[nodiscard]] std::expected<void, SceneRuntimeError> ActivateAudio();
		// Temporarily silence all started scene voices while testing candidate activation; resume on rejection.
		[[nodiscard]] std::expected<void, SceneRuntimeError> SuspendAudioForTransition();
		[[nodiscard]] std::expected<void, SceneRuntimeError> ResumeAudioAfterTransition();
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
		[[nodiscard]] std::span<const BlueprintRuntimeDiagnostic> GetBlueprintDiagnostics() const noexcept;
		// Returns bounded recent traces. Copy a trace handle and use ReadBlueprintTrace before acting on it;
		// the read rejects stopped sessions, changed program generations, and destroyed/replaced entities.
		[[nodiscard]] std::span<const BlueprintRuntimeTrace> GetBlueprintTraces() const noexcept;
		[[nodiscard]] std::optional<BlueprintRuntimeTrace> ReadBlueprintTrace(
			const BlueprintRuntimeTraceHandle& Handle) const;
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
