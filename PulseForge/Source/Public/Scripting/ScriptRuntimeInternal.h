#pragma once

#include "Scripting/ScriptRuntime.h"
#include "Scene/Entity.h"

extern "C"
{
#include <lauxlib.h>
#include <lua.h>
}

#include <array>
#include <string_view>

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
	};

	struct ScriptInstance final
	{
		Entity BoundEntity;
		AssetID Asset;
		Scene* SourceScene = nullptr;
		const Project* SourceProject = nullptr;
		std::shared_ptr<const CookedContentProvider> CookedAssets;
		ScriptRuntimeServices Services;
		LuaMemory Memory;
		lua_State* State = nullptr;
		int EnvironmentReference = LUA_NOREF;
		std::array<int, CallbackNames.size()> CallbackNameReferences{};
		int InstructionBudget = 0;
		int MaximumInstructions = 0;
		bool Faulted = false;
		bool DestroyInvoked = false;
		bool Destroying = false;

		ScriptInstance() { CallbackNameReferences.fill(LUA_NOREF); }

		~ScriptInstance()
		{
			if (State) lua_close(State);
		}
	};

	[[nodiscard]] inline ScriptInstance* GetScriptInstance(lua_State* State)
	{
		return *static_cast<ScriptInstance**>(lua_getextraspace(State));
	}
} // namespace PulseForge::ScriptRuntimeDetail
