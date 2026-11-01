#pragma once

#include "Assets/AssetID.h"
#include "Core/Core.h"
#include "Core/Timestep.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace PulseForge
{
	class AudioSceneRuntime;
	class Entity;
	class GameplayInputService;
	class Input;
	class PhysicsSceneRuntime;
	class Project;
	class Scene;
	class CookedContentProvider;
	struct PhysicsContactEvent;

	enum class ScriptRuntimeAllocationPointForTesting : uint8_t
	{
		None,
		MainChunkReference,
		ModuleChunkReference
	};

#if defined(PF_DEBUG)
	struct ScriptRuntimeAllocationFaultHooksForTesting final
	{
		void* Context = nullptr;
		bool (*RejectAllocation)(void* Context, ScriptRuntimeAllocationPointForTesting Point,
			size_t ModuleIndex, size_t RequestedBytes) noexcept = nullptr;
		size_t RegistryPaddingForTesting = 0;
	};
#endif

	struct ScriptRuntimeDesc
	{
		size_t MaxSourceBytes = 2u * 1024u * 1024u;
		size_t MaxLuaMemoryBytes = 16u * 1024u * 1024u;
		size_t MaxTotalLuaMemoryBytes = 256u * 1024u * 1024u;
		size_t MaxTotalScriptSourceBytes = 32u * 1024u * 1024u;
		size_t MaxScriptModuleCount = 128;
		int MaxInstructionsPerCallback = 100000;
		uint64_t SessionGeneration = 0;
#if defined(PF_DEBUG)
		// Optional deterministic allocator fault seam for production-path tests. The hook and its
		// context must outlive this runtime; it is unavailable in Release and Dist builds.
		const ScriptRuntimeAllocationFaultHooksForTesting* AllocationFaultHooksForTesting = nullptr;
#endif
	};

	enum class ScriptDiagnosticCode : uint8_t
	{
		InvalidComponent,
		AssetNotFound,
		WrongAssetType,
		FileReadFailed,
		LuaInitializationFailed,
		CompileFailed,
		InitializationFailed,
		UpdateFailed,
		DestroyFailed,
		ReloadRejected,
		ReloadActivationFailed,
		ModuleLoadFailed
	};

	struct ScriptDiagnostic
	{
		UUID Entity;
		AssetID ScriptAsset;
		ScriptDiagnosticCode Code;
		std::string Message;
		// Trusted project-relative path from the immutable script snapshot; never parsed for file opening.
		std::string SourceRelativePath;
		size_t Line = 0;
		// Snapshot identity keeps diagnostics from distinct source edits separate.
		std::string SourceFingerprint;
		// Structured, trusted source identity for Editor navigation. These are never parsed from Message.
		UUID SourceProject;
		AssetID SourceAsset;
	};

	enum class ScriptRuntimeCallback : uint8_t
	{
		OnCreate,
		OnUpdate,
		OnDestroy,
		OnCollisionBegin,
		OnCollisionStay,
		OnCollisionEnd,
		OnTriggerEnter,
		OnTriggerStay,
	OnTriggerExit,
	OnUIAction
	};

	enum class ScriptRuntimeObservationKind : uint8_t
	{
		ScriptAttached,
		CallbackEntered,
		CallbackCompleted,
		Diagnostic
	};

	struct ScriptRuntimeObservation
	{
		ScriptRuntimeObservationKind Kind;
		UUID Entity;
		AssetID ScriptAsset;
		ScriptRuntimeCallback Callback = ScriptRuntimeCallback::OnCreate;
		ScriptDiagnosticCode Diagnostic = ScriptDiagnosticCode::InvalidComponent;
	};

	// Optional non-owning observation hook. The context must outlive the runtime; notifications
	// are value-only and the callback must not throw because it may run during shutdown.
	struct ScriptRuntimeObserver
	{
		void* Context = nullptr;
		void (*Observe)(void* Context, const ScriptRuntimeObservation& Observation) noexcept = nullptr;
	};

	// Non-owning gameplay services. When provided, they must remain alive until ScriptRuntime stops.
	struct ScriptRuntimeServices
	{
		PhysicsSceneRuntime* Physics = nullptr;
		AudioSceneRuntime* Audio = nullptr;
		const GameplayInputService* GameplayInput = nullptr;
		Input* InputState = nullptr;
		ScriptRuntimeObserver Observer;
		std::shared_ptr<const CookedContentProvider> CookedAssets;
		std::function<bool(const Entity&)> QueueEntityDestroy;
		std::function<bool(const AssetID&)> QueueSceneTransition;
		std::function<bool()> QueueStop;
		std::function<bool(bool)> QueuePause;
		std::function<bool(UUID, std::string)> SetRuntimeUIText;
	};

	enum class ScriptRuntimeErrorCode : uint8_t
	{
		AlreadyRunning,
		NotRunning,
		DifferentScene,
		InvalidDeltaTime,
		InvalidSettings,
		InitializationFailed,
		OperationFailed
	};

	struct ScriptRuntimeError
	{
		ScriptRuntimeErrorCode Code;
		std::string Message;
	};

	// The Project and a started Scene must outlive this runtime. Lua states are isolated per scripted entity.
	class PULSEFORGE_API ScriptRuntime final
	{
	public:
		explicit ScriptRuntime(
			const Project& SourceProject,
			ScriptRuntimeDesc Description = {},
			ScriptRuntimeServices Services = {});
		explicit ScriptRuntime(
			std::shared_ptr<const CookedContentProvider> CookedAssets,
			ScriptRuntimeDesc Description = {},
			ScriptRuntimeServices Services = {});
		~ScriptRuntime();
		ScriptRuntime(const ScriptRuntime&) = delete;
		ScriptRuntime& operator=(const ScriptRuntime&) = delete;

		[[nodiscard]] std::expected<void, ScriptRuntimeError> Start(Scene& Source);
		// Owner-thread only. Captures the current script asset and entity incarnation. The immutable
		// source graph is resolved and validated later at the scene runtime safe point.
		[[nodiscard]] std::expected<uint64_t, ScriptRuntimeError> RequestReload(const Entity& Target);
		// Owner-thread only. Services queued requests without holding the queue lock through callbacks.
		[[nodiscard]] std::expected<size_t, ScriptRuntimeError> ServicePendingReloads(Scene& Source);
		[[nodiscard]] bool CancelReload(uint64_t Ticket) noexcept;
		void Stop() noexcept;
		[[nodiscard]] bool IsRunning() const noexcept;
		[[nodiscard]] size_t GetScriptCount() const noexcept;
		[[nodiscard]] std::span<const ScriptDiagnostic> GetDiagnostics() const noexcept;
		void ClearDiagnostics() noexcept;
		[[nodiscard]] std::expected<void, ScriptRuntimeError> Advance(Scene& Source, Timestep DeltaTime);
		// Returns false when a host accepted a Stop/transition request and the current event batch must stop.
		[[nodiscard]] bool DispatchPhysicsContact(Scene& Source, const PhysicsContactEvent& Event) noexcept;
		[[nodiscard]] bool DispatchRuntimeUIAction(Scene& Source, const Entity& Recipient, std::string_view Action) noexcept;

	private:
		struct Impl;
		const Project* m_Project = nullptr;
		ScriptRuntimeDesc m_Description;
		ScriptRuntimeServices m_Services;
		std::unique_ptr<Impl> m_Impl;
		std::vector<ScriptDiagnostic> m_StoppedDiagnostics;
	};
}
