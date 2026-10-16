#pragma once

struct lua_State;

namespace PulseForge::ScriptEnvironment
{
	int OpenRestrictedLibraries(lua_State* State);
	int Initialize(lua_State* State);
} // namespace PulseForge::ScriptEnvironment
