#pragma once

#include <string_view>

struct lua_State;

namespace PulseForge::ScriptEnvironment
{
	int OpenRestrictedLibraries(lua_State* State);
	int Initialize(lua_State* State);
	int ExecutePreparedChunk(lua_State* State);
	void PushChunkEnvironment(lua_State* State, std::string_view ImporterPath);
} // namespace PulseForge::ScriptEnvironment
