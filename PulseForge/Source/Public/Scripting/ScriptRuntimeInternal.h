#pragma once

#include "Scripting/ScriptRuntime.h"
#include "Assets/ScriptSourceMetadata.h"
#include "Scene/Entity.h"

extern "C"
{
#include <lauxlib.h>
#include <lua.h>
}

#include <array>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace PulseForge::ScriptRuntimeDetail
{
	inline constexpr int InstructionHookQuantum = 1000;
	inline constexpr std::array<std::string_view, 3> CallbackNames = { "OnCreate", "OnUpdate", "OnDestroy" };

	struct LuaMemoryPool final
	{
		size_t UsedBytes = 0;
		size_t MaximumBytes = 0;
	};

	struct LuaMemory final
	{
		size_t UsedBytes = 0;
		size_t MaximumBytes = 0;
		LuaMemoryPool* Pool = nullptr;
#if defined(PF_DEBUG)
		const ScriptRuntimeAllocationFaultHooksForTesting* FaultHooks = nullptr;
		ScriptRuntimeAllocationPointForTesting ActiveReferencePoint = ScriptRuntimeAllocationPointForTesting::None;
		size_t ActiveModuleIndex = 0;
#endif
	};

	struct ScriptModuleImport final
	{
		std::string DeclaredName;
		std::string TargetPath;
	};

	struct ScriptModuleSource final
	{
		AssetID SourceAsset;
		std::string ProjectRelativePath; // Canonical extensionless module identity.
		std::string SourceRelativePath; // Canonical project-relative .lua path used for imports.
		std::string Source;
		std::vector<ScriptModuleImport> Imports;
	};

	enum class ScriptModuleLoadState : uint8_t
	{
		Unloaded,
		Loading,
		Loaded
	};

	struct ScriptModuleRuntime final
	{
		std::string ProjectRelativePath;
		std::vector<ScriptModuleImport> Imports;
		int CompiledChunkReference = LUA_NOREF;
		int ValueReference = LUA_NOREF;
		ScriptModuleLoadState State = ScriptModuleLoadState::Unloaded;
	};

	// Fully read and validated before a script attachment can be replaced. Paths are project-relative
	// identities only; Lua receives no filesystem access through this snapshot.
	struct ScriptSourceSnapshot final
	{
		AssetID ScriptAsset;
		UUID SourceProject;
		std::string Fingerprint;
		std::string MainSourcePath;
		std::string MainSource;
		ScriptSourceMetadata Metadata;
		std::vector<ScriptModuleImport> MainImports;
		std::vector<ScriptModuleSource> Modules;
	};

	struct ScriptInstance final
	{
		Entity BoundEntity;
		AssetID Asset;
		std::shared_ptr<const ScriptSourceSnapshot> SourceSnapshot;
		Scene* SourceScene = nullptr;
		const Project* SourceProject = nullptr;
		std::shared_ptr<const CookedContentProvider> CookedAssets;
		ScriptRuntimeServices Services;
		LuaMemory Memory;
		lua_State* State = nullptr;
		int EnvironmentReference = LUA_NOREF;
		std::array<int, CallbackNames.size()> CallbackNameReferences{};
		std::vector<ScriptModuleRuntime> Modules;
		int MainChunkReference = LUA_NOREF;
		int ExecuteChunkCallbackReference = LUA_NOREF;
		int ExecutingChunkReference = LUA_NOREF;
		const char* ExecutingImporterPath = nullptr;
		size_t ExecutingImporterPathLength = 0;
		int InstructionBudget = 0;
		int MaximumInstructions = 0;
		bool Faulted = false;
		bool DestroyInvoked = false;
		bool Destroying = false;
		bool ModuleFailurePending = false;
		std::array<char, 512> ModuleFailureMessage{};

		ScriptInstance() { CallbackNameReferences.fill(LUA_NOREF); }

		~ScriptInstance()
		{
			if (State) lua_close(State);
		}
	};

	struct PendingScriptReload final
	{
		uint64_t Ticket = 0;
		uint64_t SessionGeneration = 0;
		AssetID ScriptAsset;
		std::string SourceFingerprint;
		Entity Target;
	};

	[[nodiscard]] inline ScriptInstance* GetScriptInstance(lua_State* State)
	{
		return *static_cast<ScriptInstance**>(lua_getextraspace(State));
	}
} // namespace PulseForge::ScriptRuntimeDetail
