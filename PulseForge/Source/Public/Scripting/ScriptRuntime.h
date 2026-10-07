#pragma once

#include "Assets/AssetID.h"
#include "Core/Core.h"
#include "Core/Timestep.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace PulseForge
{
	class Project;
	class Scene;

	struct ScriptRuntimeDesc
	{
		size_t MaxSourceBytes = 2u * 1024u * 1024u;
		size_t MaxLuaMemoryBytes = 16u * 1024u * 1024u;
		size_t MaxTotalLuaMemoryBytes = 256u * 1024u * 1024u;
		int MaxInstructionsPerCallback = 100000;
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
		DestroyFailed
	};

	struct ScriptDiagnostic
	{
		UUID Entity;
		AssetID ScriptAsset;
		ScriptDiagnosticCode Code;
		std::string Message;
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
		explicit ScriptRuntime(const Project& SourceProject, ScriptRuntimeDesc Description = {});
		~ScriptRuntime();
		ScriptRuntime(const ScriptRuntime&) = delete;
		ScriptRuntime& operator=(const ScriptRuntime&) = delete;

		[[nodiscard]] std::expected<void, ScriptRuntimeError> Start(Scene& Source);
		void Stop() noexcept;
		[[nodiscard]] bool IsRunning() const noexcept;
		[[nodiscard]] size_t GetScriptCount() const noexcept;
		[[nodiscard]] std::span<const ScriptDiagnostic> GetDiagnostics() const noexcept;
		void ClearDiagnostics() noexcept;
		[[nodiscard]] std::expected<void, ScriptRuntimeError> Advance(Scene& Source, Timestep DeltaTime);

	private:
		struct Impl;
		const Project& m_Project;
		ScriptRuntimeDesc m_Description;
		std::unique_ptr<Impl> m_Impl;
		std::vector<ScriptDiagnostic> m_StoppedDiagnostics;
	};
}
