#include "Core/PulseForgePCH.h"
#include "Scripting/ScriptRuntime.h"

#include "Assets/AssetPathResolver.h"
#include "Assets/PrefabAssetService.h"
#include "Assets/Project.h"
#include "Audio/AudioSceneRuntime.h"
#include "Core/Log.h"
#include "Core/Input.h"
#include "Physics/PhysicsSceneRuntime.h"
#include "Scene/Components/ScriptComponent.h"
#include "Scene/Entity.h"
#include "Scene/Scene.h"

extern "C"
{
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <system_error>
#include <string_view>
#include <utility>

namespace PulseForge
{
	namespace
	{
		constexpr char EntityMetatableName[] = "PulseForge.Entity";
		constexpr int InstructionHookQuantum = 1000;
		constexpr std::array<std::string_view, 3> CallbackNames = { "OnCreate", "OnUpdate", "OnDestroy" };

		ScriptRuntimeError MakeRuntimeError(ScriptRuntimeErrorCode Code, std::string Message)
		{
			return { Code, std::move(Message) };
		}

		ScriptDiagnostic MakeDiagnostic(
			UUID EntityIdentifier,
			AssetID ScriptAsset,
			ScriptDiagnosticCode Code,
			std::string Message)
		{
			return { EntityIdentifier, ScriptAsset, Code, std::move(Message) };
		}

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
			ScriptRuntimeServices Services;
			LuaMemory Memory;
			lua_State* State = nullptr;
			int EnvironmentReference = LUA_NOREF;
			std::array<int, CallbackNames.size()> CallbackNameReferences{};
			int InstructionBudget = 0;
			int MaximumInstructions = 0;
			bool Faulted = false;

			ScriptInstance()
			{
				CallbackNameReferences.fill(LUA_NOREF);
			}

			~ScriptInstance()
			{
				if (State)
					lua_close(State);
			}
		};

		struct LuaEntity final
		{
			Entity Value;
		};

		void* BoundedLuaAllocator(void* UserData, void* Pointer, size_t OldSize, size_t NewSize)
		{
			auto& Memory = *static_cast<LuaMemory*>(UserData);
			if (NewSize == 0)
			{
				std::free(Pointer);
				if (Pointer)
				{
					const size_t ReleasedBytes = std::min(OldSize, Memory.UsedBytes);
					Memory.UsedBytes -= ReleasedBytes;
					if (Memory.Pool)
						Memory.Pool->UsedBytes -= std::min(ReleasedBytes, Memory.Pool->UsedBytes);
				}
				return nullptr;
			}

			const size_t PreviousSize = Pointer ? std::min(OldSize, Memory.UsedBytes) : 0;
			const size_t BaseSize = Memory.UsedBytes - PreviousSize;
			if (BaseSize > Memory.MaximumBytes || NewSize > Memory.MaximumBytes - BaseSize)
				return nullptr;
			const size_t PoolBaseSize = Memory.Pool
				? Memory.Pool->UsedBytes - std::min(PreviousSize, Memory.Pool->UsedBytes)
				: 0;
			if (Memory.Pool &&
				(PoolBaseSize > Memory.Pool->MaximumBytes || NewSize > Memory.Pool->MaximumBytes - PoolBaseSize))
				return nullptr;

			void* Replacement = std::realloc(Pointer, NewSize);
			if (Replacement)
			{
				Memory.UsedBytes = BaseSize + NewSize;
				if (Memory.Pool)
					Memory.Pool->UsedBytes = PoolBaseSize + NewSize;
			}
			return Replacement;
		}

		ScriptInstance* GetScriptInstance(lua_State* State)
		{
			return *static_cast<ScriptInstance**>(lua_getextraspace(State));
		}

		void InstructionBudgetHook(lua_State* State, lua_Debug*)
		{
			ScriptInstance* Instance = GetScriptInstance(State);
			if (!Instance)
				return;
			if (Instance->InstructionBudget <= InstructionHookQuantum)
			{
				Instance->InstructionBudget = 0;
				lua_pushliteral(State, "script instruction budget exceeded");
				(void)lua_error(State);
				return;
			}
			Instance->InstructionBudget -= InstructionHookQuantum;
		}

		int ReturnHostError(lua_State* State, const char* Message)
		{
			lua_pushnil(State);
			lua_pushstring(State, Message);
			return 2;
		}

		LuaEntity* GetLuaEntity(lua_State* State)
		{
			return static_cast<LuaEntity*>(luaL_testudata(State, 1, EntityMetatableName));
		}

		int LuaEntityCollect(lua_State* State)
		{
			if (auto* UserData = GetLuaEntity(State))
				std::destroy_at(UserData);
			return 0;
		}

		void SetEntityMetatable(lua_State* State)
		{
			luaL_getmetatable(State, EntityMetatableName);
			lua_setmetatable(State, -2);
		}

		LuaEntity* CreateLuaEntityUserData(lua_State* State)
		{
			auto* UserData = static_cast<LuaEntity*>(lua_newuserdatauv(State, sizeof(LuaEntity), 0));
			new (UserData) LuaEntity{};
			SetEntityMetatable(State);
			return UserData;
		}

		template<size_t Capacity>
		void CopyHostError(std::array<char, Capacity>& Buffer, std::string_view Message)
		{
			const size_t CopyLength = std::min(Message.size(), Capacity - 1);
			std::copy_n(Message.data(), CopyLength, Buffer.data());
			Buffer[CopyLength] = '\0';
		}

		int LuaEntityUUID(lua_State* State)
		{
			LuaEntity* UserData = GetLuaEntity(State);
			if (!UserData)
				return ReturnHostError(State, "entity method requires an Entity value as self");

			constexpr char HexDigits[] = "0123456789abcdef";
			std::array<char, 36> Identifier{};
			const UUID Value = UserData->Value.GetUUID();
			size_t TextIndex = 0;
			for (size_t ByteIndex = 0; ByteIndex < 16; ++ByteIndex)
			{
				if (ByteIndex == 4 || ByteIndex == 6 || ByteIndex == 8 || ByteIndex == 10)
					Identifier[TextIndex++] = '-';
				const uint64_t Word = ByteIndex < 8 ? Value.GetHigh() : Value.GetLow();
				const size_t WordByteIndex = ByteIndex % 8;
				const auto Byte = static_cast<uint8_t>((Word >> ((7 - WordByteIndex) * 8)) & 0xff);
				Identifier[TextIndex++] = HexDigits[Byte >> 4];
				Identifier[TextIndex++] = HexDigits[Byte & 0x0f];
			}
			lua_pushlstring(State, Identifier.data(), Identifier.size());
			return 1;
		}

		int LuaEntityGetTranslation(lua_State* State)
		{
			LuaEntity* UserData = GetLuaEntity(State);
			if (!UserData)
				return ReturnHostError(State, "entity method requires an Entity value as self");

			glm::vec3 Translation{};
			bool HasTransform = false;
			{
				const auto Transform = UserData->Value.GetTransform();
				if (Transform)
				{
					Translation = Transform->Translation;
					HasTransform = true;
				}
			}
			if (!HasTransform)
				return ReturnHostError(State, "entity is no longer valid in its scene");

			lua_pushnumber(State, Translation.x);
			lua_pushnumber(State, Translation.y);
			lua_pushnumber(State, Translation.z);
			return 3;
		}

		int LuaEntitySetTranslation(lua_State* State)
		{
			LuaEntity* UserData = GetLuaEntity(State);
			if (!UserData)
				return ReturnHostError(State, "entity method requires an Entity value as self");

			int IsXNumber = 0;
			int IsYNumber = 0;
			int IsZNumber = 0;
			const lua_Number X = lua_tonumberx(State, 2, &IsXNumber);
			const lua_Number Y = lua_tonumberx(State, 3, &IsYNumber);
			const lua_Number Z = lua_tonumberx(State, 4, &IsZNumber);
			if (!IsXNumber || !IsYNumber || !IsZNumber ||
				!std::isfinite(X) || !std::isfinite(Y) || !std::isfinite(Z))
				return ReturnHostError(State, "set_translation requires three finite numbers");

			const char* Failure = nullptr;
			{
				auto Transform = UserData->Value.GetTransform();
				if (!Transform)
					Failure = "entity is no longer valid in its scene";
				else
				{
					Transform->Translation = { static_cast<float>(X), static_cast<float>(Y), static_cast<float>(Z) };
					if (!std::isfinite(Transform->Translation.x) || !std::isfinite(Transform->Translation.y) ||
						!std::isfinite(Transform->Translation.z))
						Failure = "translation values are outside the supported range";
					else if (!UserData->Value.SetTransform(*Transform))
						Failure = "scene rejected the entity transform";
				}
			}
			if (Failure)
				return ReturnHostError(State, Failure);
			lua_pushboolean(State, true);
			return 1;
		}

		int LuaEntityApplyForce(lua_State* State)
		{
			LuaEntity* UserData = GetLuaEntity(State);
			if (!UserData)
				return ReturnHostError(State, "entity method requires an Entity value as self");
			ScriptInstance* Instance = GetScriptInstance(State);
			if (!Instance || !Instance->Services.Physics)
				return ReturnHostError(State, "physics service is unavailable to this script runtime");

			int IsXNumber = 0;
			int IsYNumber = 0;
			int IsZNumber = 0;
			const lua_Number X = lua_tonumberx(State, 2, &IsXNumber);
			const lua_Number Y = lua_tonumberx(State, 3, &IsYNumber);
			const lua_Number Z = lua_tonumberx(State, 4, &IsZNumber);
			if (!IsXNumber || !IsYNumber || !IsZNumber ||
				!std::isfinite(X) || !std::isfinite(Y) || !std::isfinite(Z))
				return ReturnHostError(State, "apply_force requires three finite numbers");

			const glm::vec3 Force{ static_cast<float>(X), static_cast<float>(Y), static_cast<float>(Z) };
			if (!std::isfinite(Force.x) || !std::isfinite(Force.y) || !std::isfinite(Force.z))
				return ReturnHostError(State, "force values are outside the supported range");
			if (auto Result = Instance->Services.Physics->ApplyForce(UserData->Value.GetUUID(), Force); !Result)
				return ReturnHostError(State, Result.error().Message.c_str());
			lua_pushboolean(State, true);
			return 1;
		}

		using AudioControl = std::expected<void, AudioSceneRuntimeError> (AudioSceneRuntime::*)(UUID);

		int InvokeAudioControl(lua_State* State, AudioControl Control)
		{
			LuaEntity* UserData = GetLuaEntity(State);
			if (!UserData)
				return ReturnHostError(State, "entity method requires an Entity value as self");
			ScriptInstance* Instance = GetScriptInstance(State);
			if (!Instance || !Instance->Services.Audio)
				return ReturnHostError(State, "audio service is unavailable to this script runtime");

			if (auto Result = (Instance->Services.Audio->*Control)(UserData->Value.GetUUID()); !Result)
				return ReturnHostError(State, Result.error().Message.c_str());
			lua_pushboolean(State, true);
			return 1;
		}

		bool ReadIntegerArgument(lua_State* State, int Index, int& Value)
		{
			int IsInteger = 0;
			const lua_Integer LuaValue = lua_tointegerx(State, Index, &IsInteger);
			if (!IsInteger || LuaValue < std::numeric_limits<int>::min() || LuaValue > std::numeric_limits<int>::max())
				return false;
			Value = static_cast<int>(LuaValue);
			return true;
		}

		int LuaInputIsKeyPressed(lua_State* State)
		{
			ScriptInstance* Instance = GetScriptInstance(State);
			if (!Instance || !Instance->Services.InputState)
				return ReturnHostError(State, "input service is unavailable to this script runtime");

			int KeyCode = 0;
			if (!ReadIntegerArgument(State, 1, KeyCode))
				return ReturnHostError(State, "input.is_key_pressed requires an integer key code");
			lua_pushboolean(State, Instance->Services.InputState->IsKeyPressed(KeyCode));
			return 1;
		}

		int LuaInputIsMouseButtonPressed(lua_State* State)
		{
			ScriptInstance* Instance = GetScriptInstance(State);
			if (!Instance || !Instance->Services.InputState)
				return ReturnHostError(State, "input service is unavailable to this script runtime");

			int Button = 0;
			if (!ReadIntegerArgument(State, 1, Button))
				return ReturnHostError(State, "input.is_mouse_button_pressed requires an integer button code");
			lua_pushboolean(State, Instance->Services.InputState->IsMouseButtonPressed(Button));
			return 1;
		}

		int LuaInputGetMousePosition(lua_State* State)
		{
			ScriptInstance* Instance = GetScriptInstance(State);
			if (!Instance || !Instance->Services.InputState)
				return ReturnHostError(State, "input service is unavailable to this script runtime");

			const MousePosition Position = Instance->Services.InputState->GetMousePosition();
			lua_pushnumber(State, Position.X);
			lua_pushnumber(State, Position.Y);
			return 2;
		}

		int LuaEntityPlayAudio(lua_State* State)
		{
			return InvokeAudioControl(State, &AudioSceneRuntime::Play);
		}

		int LuaEntityPauseAudio(lua_State* State)
		{
			return InvokeAudioControl(State, &AudioSceneRuntime::Pause);
		}

		int LuaEntityResumeAudio(lua_State* State)
		{
			return InvokeAudioControl(State, &AudioSceneRuntime::Resume);
		}

		int LuaEntityStopAudio(lua_State* State)
		{
			return InvokeAudioControl(State, &AudioSceneRuntime::StopPlayback);
		}

		int LuaEntityDestroy(lua_State* State)
		{
			LuaEntity* UserData = GetLuaEntity(State);
			if (!UserData)
				return ReturnHostError(State, "entity method requires an Entity value as self");
			ScriptInstance* Instance = GetScriptInstance(State);
			if (!Instance || !Instance->SourceScene)
				return ReturnHostError(State, "scene service is no longer available");

			std::array<char, 512> Failure{};
			bool Destroyed = false;
			try
			{
				{
					auto Result = Instance->SourceScene->DestroyEntity(UserData->Value);
					if (Result)
						Destroyed = true;
					else
						CopyHostError(Failure, Result.error().Message);
				}
			}
			catch (const std::exception& Exception)
			{
				CopyHostError(Failure, Exception.what());
			}
			catch (...)
			{
				CopyHostError(Failure, "Could not destroy the scene entity");
			}
			if (!Destroyed)
				return ReturnHostError(State, Failure.data());
			lua_pushboolean(State, true);
			return 1;
		}

		std::optional<UUID> ReadUUIDArgument(lua_State* State, int Index)
		{
			if (lua_type(State, Index) != LUA_TSTRING)
				return std::nullopt;
			size_t Length = 0;
			const char* Text = lua_tolstring(State, Index, &Length);
			const auto Parsed = UUID::Parse(std::string_view(Text, Length));
			if (!Parsed)
				return std::nullopt;
			return *Parsed;
		}

		int LuaSceneFindEntity(lua_State* State)
		{
			ScriptInstance* Instance = GetScriptInstance(State);
			if (!Instance || !Instance->SourceScene)
				return ReturnHostError(State, "scene service is no longer available");
			const auto Identifier = ReadUUIDArgument(State, 1);
			if (!Identifier)
				return ReturnHostError(State, "scene.find_entity requires a canonical UUID");

			auto* UserData = CreateLuaEntityUserData(State);
			bool Found = false;
			{
				const auto FoundEntity = Instance->SourceScene->FindEntity(*Identifier);
				if (FoundEntity)
				{
					UserData->Value = *FoundEntity;
					Found = true;
				}
			}
			if (!Found)
			{
				lua_pop(State, 1);
				lua_pushnil(State);
				return 1;
			}
			return 1;
		}

		int LuaSceneCreateEntity(lua_State* State)
		{
			ScriptInstance* Instance = GetScriptInstance(State);
			if (!Instance || !Instance->SourceScene)
				return ReturnHostError(State, "scene service is no longer available");
			if (lua_type(State, 1) != LUA_TSTRING)
				return ReturnHostError(State, "scene.create_entity requires a name string");

			size_t NameLength = 0;
			const char* NameText = lua_tolstring(State, 1, &NameLength);
			auto* UserData = CreateLuaEntityUserData(State);
			std::array<char, 512> Failure{};
			bool Created = false;
			try
			{
				{
					std::string Name(NameText, NameLength);
					auto Result = Instance->SourceScene->CreateEntity(std::move(Name));
					if (Result)
					{
						UserData->Value = std::move(*Result);
						Created = true;
					}
					else
						CopyHostError(Failure, Result.error().Message);
				}
			}
			catch (const std::exception& Exception)
			{
				CopyHostError(Failure, Exception.what());
			}
			catch (...)
			{
				CopyHostError(Failure, "Could not create a scene entity");
			}
			if (!Created)
			{
				lua_pop(State, 1);
				return ReturnHostError(State, Failure.data());
			}
			return 1;
		}

		int LuaSceneSpawnPrefab(lua_State* State)
		{
			ScriptInstance* Instance = GetScriptInstance(State);
			if (!Instance || !Instance->SourceScene || !Instance->SourceProject)
				return ReturnHostError(State, "scene or project service is no longer available");
			const auto PrefabIdentifier = ReadUUIDArgument(State, 1);
			if (!PrefabIdentifier)
				return ReturnHostError(State, "scene.spawn_prefab requires a canonical prefab asset UUID");

			auto* UserData = CreateLuaEntityUserData(State);
			std::array<char, 512> Failure{};
			bool Spawned = false;
			try
			{
				{
					auto Result = PrefabAssetService::Instantiate(
						*PrefabIdentifier,
						Instance->SourceProject->GetRootPath(),
						Instance->SourceProject->GetAssetRegistry(),
						*Instance->SourceScene);
					if (Result)
					{
						UserData->Value = std::move(*Result);
						Spawned = true;
					}
					else
						CopyHostError(Failure, Result.error().Message);
				}
			}
			catch (const std::exception& Exception)
			{
				CopyHostError(Failure, Exception.what());
			}
			catch (...)
			{
				CopyHostError(Failure, "Could not instantiate the prefab");
			}
			if (!Spawned)
			{
				lua_pop(State, 1);
				return ReturnHostError(State, Failure.data());
			}
			return 1;
		}

		int OpenScriptLibraries(lua_State* State)
		{
			luaL_requiref(State, LUA_GNAME, luaopen_base, 1);
			lua_pop(State, 1);
			luaL_requiref(State, LUA_TABLIBNAME, luaopen_table, 1);
			lua_pop(State, 1);
			luaL_requiref(State, LUA_STRLIBNAME, luaopen_string, 1);
			lua_pop(State, 1);
			luaL_requiref(State, LUA_MATHLIBNAME, luaopen_math, 1);
			lua_pop(State, 1);
			luaL_requiref(State, LUA_UTF8LIBNAME, luaopen_utf8, 1);
			lua_pop(State, 1);

			lua_pushnil(State);
			lua_setglobal(State, "dofile");
			lua_pushnil(State);
			lua_setglobal(State, "loadfile");
			lua_pushnil(State);
			lua_setglobal(State, "load");
			lua_pushnil(State);
			lua_setglobal(State, "collectgarbage");
			// Protected calls could swallow instruction-hook errors and keep a runaway callback alive.
			lua_pushnil(State);
			lua_setglobal(State, "pcall");
			lua_pushnil(State);
			lua_setglobal(State, "xpcall");
			lua_getglobal(State, "string");
			lua_pushnil(State);
			lua_setfield(State, -2, "dump");
			lua_pop(State, 1);
			return 0;
		}

		int InitializeScriptEnvironment(lua_State* State)
		{
			ScriptInstance* Instance = GetScriptInstance(State);
			if (!Instance || !Instance->SourceScene)
				return 0;

			luaL_newmetatable(State, EntityMetatableName);
			lua_pushboolean(State, 0);
			lua_setfield(State, -2, "__metatable");
			lua_pushcfunction(State, LuaEntityCollect);
			lua_setfield(State, -2, "__gc");
			lua_newtable(State);
			lua_pushcfunction(State, LuaEntityUUID);
			lua_setfield(State, -2, "uuid");
			lua_pushcfunction(State, LuaEntityDestroy);
			lua_setfield(State, -2, "destroy");
			lua_pushcfunction(State, LuaEntityGetTranslation);
			lua_setfield(State, -2, "get_translation");
			lua_pushcfunction(State, LuaEntitySetTranslation);
			lua_setfield(State, -2, "set_translation");
			lua_pushcfunction(State, LuaEntityApplyForce);
			lua_setfield(State, -2, "apply_force");
			lua_pushcfunction(State, LuaEntityPlayAudio);
			lua_setfield(State, -2, "play_audio");
			lua_pushcfunction(State, LuaEntityPauseAudio);
			lua_setfield(State, -2, "pause_audio");
			lua_pushcfunction(State, LuaEntityResumeAudio);
			lua_setfield(State, -2, "resume_audio");
			lua_pushcfunction(State, LuaEntityStopAudio);
			lua_setfield(State, -2, "stop_audio");
			lua_setfield(State, -2, "__index");
			lua_pop(State, 1);

			lua_newtable(State);
			lua_pushcfunction(State, LuaSceneFindEntity);
			lua_setfield(State, -2, "find_entity");
			lua_pushcfunction(State, LuaSceneCreateEntity);
			lua_setfield(State, -2, "create_entity");
			lua_pushcfunction(State, LuaSceneSpawnPrefab);
			lua_setfield(State, -2, "spawn_prefab");
			lua_setglobal(State, "scene");

			lua_newtable(State);
			lua_pushcfunction(State, LuaInputIsKeyPressed);
			lua_setfield(State, -2, "is_key_pressed");
			lua_pushcfunction(State, LuaInputIsMouseButtonPressed);
			lua_setfield(State, -2, "is_mouse_button_pressed");
			lua_pushcfunction(State, LuaInputGetMousePosition);
			lua_setfield(State, -2, "get_mouse_position");
			lua_setglobal(State, "input");

			lua_newuserdatauv(State, sizeof(LuaEntity), 0);
			luaL_getmetatable(State, EntityMetatableName);
			lua_setmetatable(State, -2);
			auto* UserData = static_cast<LuaEntity*>(lua_touserdata(State, -1));
			new (UserData) LuaEntity{ Instance->BoundEntity };
			lua_setglobal(State, "entity");

			lua_pushglobaltable(State);
			Instance->EnvironmentReference = luaL_ref(State, LUA_REGISTRYINDEX);
			for (size_t Index = 0; Index < CallbackNames.size(); ++Index)
			{
				const std::string_view Name = CallbackNames[Index];
				lua_pushlstring(State, Name.data(), Name.size());
				Instance->CallbackNameReferences[Index] = luaL_ref(State, LUA_REGISTRYINDEX);
			}
			return 0;
		}

		std::string LuaErrorMessage(lua_State* State)
		{
			const char* Message = lua_tostring(State, -1);
			std::string Result = Message ? Message : "Lua returned a non-string error object";
			lua_pop(State, 1);
			return Result;
		}

		std::expected<void, std::string> RunCallback(
			ScriptInstance& Instance,
			size_t CallbackIndex,
			std::optional<double> DeltaSeconds = std::nullopt)
		{
			lua_State* State = Instance.State;
			if (CallbackIndex >= Instance.CallbackNameReferences.size())
				return std::unexpected(std::string("Invalid script lifecycle callback index"));
			if (!lua_checkstack(State, 3))
				return std::unexpected(std::string("Lua stack limit reached before lifecycle callback"));

			lua_rawgeti(State, LUA_REGISTRYINDEX, Instance.EnvironmentReference);
			lua_rawgeti(State, LUA_REGISTRYINDEX, Instance.CallbackNameReferences[CallbackIndex]);
			lua_rawget(State, -2);
			lua_remove(State, -2);
			if (lua_isnil(State, -1))
			{
				lua_pop(State, 1);
				return {};
			}
			if (!lua_isfunction(State, -1))
			{
				lua_pop(State, 1);
				std::string Message(CallbackNames[CallbackIndex]);
				Message += " must be a function when defined";
				return std::unexpected(std::move(Message));
			}

			Instance.InstructionBudget =
				(Instance.MaximumInstructions / InstructionHookQuantum) * InstructionHookQuantum;
			lua_sethook(State, InstructionBudgetHook, LUA_MASKCOUNT, InstructionHookQuantum);
			int ArgumentCount = 0;
			if (DeltaSeconds)
			{
				lua_pushnumber(State, *DeltaSeconds);
				ArgumentCount = 1;
			}
			const int Status = lua_pcall(State, ArgumentCount, 0, 0);
			lua_sethook(State, nullptr, 0, 0);
			if (Status != LUA_OK)
				return std::unexpected(LuaErrorMessage(State));
			return {};
		}

		std::expected<std::string, ScriptDiagnostic> ReadScriptSource(
			const Project& SourceProject,
			const AssetID& ScriptAsset,
			UUID EntityIdentifier,
			size_t MaximumSourceBytes)
		{
			const auto Record = SourceProject.GetAssetRegistry().Find(ScriptAsset);
			if (!Record)
			{
				return std::unexpected(MakeDiagnostic(
					EntityIdentifier,
					ScriptAsset,
					ScriptDiagnosticCode::AssetNotFound,
					"Script UUID is not present in the project asset registry"));
			}
			if (Record->ProjectRelativePath.extension() != ".lua")
			{
				return std::unexpected(MakeDiagnostic(
					EntityIdentifier,
					ScriptAsset,
					ScriptDiagnosticCode::WrongAssetType,
					"Script asset must reference a .lua source file"));
			}

			const auto ResolvedPath = AssetPathResolver::ResolveManagedSourcePath(
				SourceProject.GetRootPath(),
				Record->ProjectRelativePath);
			if (!ResolvedPath)
			{
				return std::unexpected(MakeDiagnostic(
					EntityIdentifier,
					ScriptAsset,
					ScriptDiagnosticCode::FileReadFailed,
					ResolvedPath.error().Message));
			}

			try
			{
				std::error_code FileError;
				const uintmax_t FileSize = std::filesystem::file_size(*ResolvedPath, FileError);
				if (FileError || FileSize > MaximumSourceBytes ||
					FileSize > static_cast<uintmax_t>(std::numeric_limits<size_t>::max()) ||
					FileSize > static_cast<uintmax_t>(std::numeric_limits<std::streamsize>::max()))
				{
					return std::unexpected(MakeDiagnostic(
						EntityIdentifier,
						ScriptAsset,
						ScriptDiagnosticCode::FileReadFailed,
						FileError ? "Could not determine script source size: " + FileError.message()
							: "Script source exceeds the configured size limit"));
				}

				std::ifstream Input(*ResolvedPath, std::ios::binary);
				if (!Input.is_open())
					return std::unexpected(MakeDiagnostic(
						EntityIdentifier,
						ScriptAsset,
						ScriptDiagnosticCode::FileReadFailed,
						"Could not open the managed script source file"));

				std::string Source(static_cast<size_t>(FileSize), '\0');
				if (!Source.empty())
				{
					Input.read(Source.data(), static_cast<std::streamsize>(Source.size()));
					if (!Input || Input.bad())
						return std::unexpected(MakeDiagnostic(
							EntityIdentifier,
							ScriptAsset,
							ScriptDiagnosticCode::FileReadFailed,
							"Could not read the complete managed script source file"));
				}
				return Source;
			}
			catch (const std::exception& Exception)
			{
				return std::unexpected(MakeDiagnostic(
					EntityIdentifier,
					ScriptAsset,
					ScriptDiagnosticCode::FileReadFailed,
					std::string("Could not read script source: ") + Exception.what()));
			}
		}
	}

	struct ScriptRuntime::Impl final
	{
		const Project& SourceProject;
		ScriptRuntimeDesc Description;
		ScriptRuntimeServices Services;
		Scene* Source = nullptr;
		LuaMemoryPool MemoryPool;
		std::map<UUID, std::unique_ptr<ScriptInstance>> Scripts;
		std::map<UUID, AssetID> FailedAttachments;
		std::vector<ScriptDiagnostic> Diagnostics;

		Impl(const Project& ProjectValue, ScriptRuntimeDesc RuntimeDescription, ScriptRuntimeServices RuntimeServices)
			: SourceProject(ProjectValue), Description(RuntimeDescription), Services(RuntimeServices)
		{
			MemoryPool.MaximumBytes = Description.MaxTotalLuaMemoryBytes;
		}

		void RecordDiagnostic(ScriptDiagnostic Diagnostic)
		{
			PF_ERROR(
				"Script on entity {0} (asset {1}) failed: {2}",
				Diagnostic.Entity.ToString(),
				Diagnostic.ScriptAsset.ToString(),
				Diagnostic.Message);
			Diagnostics.push_back(std::move(Diagnostic));
		}

		std::expected<std::unique_ptr<ScriptInstance>, ScriptDiagnostic> CreateInstance(
			Scene& SceneValue,
			const Entity& EntityValue,
			const ScriptComponent& Component)
		{
			const UUID EntityIdentifier = EntityValue.GetUUID();
			if (auto Validation = Component.Validate(); !Validation)
			{
				return std::unexpected(MakeDiagnostic(
					EntityIdentifier,
					Component.ScriptAsset,
					ScriptDiagnosticCode::InvalidComponent,
					Validation.error().Message));
			}

			auto LoadedSource = ReadScriptSource(
				SourceProject,
				Component.ScriptAsset,
				EntityIdentifier,
				Description.MaxSourceBytes);
			if (!LoadedSource)
				return std::unexpected(std::move(LoadedSource.error()));
			const std::string& ScriptSource = *LoadedSource;

			auto Instance = std::make_unique<ScriptInstance>();
			Instance->BoundEntity = EntityValue;
			Instance->Asset = Component.ScriptAsset;
			Instance->SourceScene = &SceneValue;
			Instance->SourceProject = &SourceProject;
			Instance->Services = Services;
			Instance->Memory.MaximumBytes = Description.MaxLuaMemoryBytes;
			Instance->Memory.Pool = &MemoryPool;
			Instance->MaximumInstructions = Description.MaxInstructionsPerCallback;
			Instance->State = lua_newstate(BoundedLuaAllocator, &Instance->Memory, luaL_makeseed(nullptr));
			if (!Instance->State)
			{
				return std::unexpected(MakeDiagnostic(
					EntityIdentifier,
					Component.ScriptAsset,
					ScriptDiagnosticCode::LuaInitializationFailed,
					"Could not create a Lua state within the configured memory limit"));
			}
			*static_cast<ScriptInstance**>(lua_getextraspace(Instance->State)) = Instance.get();

			lua_pushcfunction(Instance->State, OpenScriptLibraries);
			if (lua_pcall(Instance->State, 0, 0, 0) != LUA_OK)
			{
				return std::unexpected(MakeDiagnostic(
					EntityIdentifier,
					Component.ScriptAsset,
					ScriptDiagnosticCode::LuaInitializationFailed,
					LuaErrorMessage(Instance->State)));
			}

			lua_pushcfunction(Instance->State, InitializeScriptEnvironment);
			if (lua_pcall(Instance->State, 0, 0, 0) != LUA_OK)
			{
				return std::unexpected(MakeDiagnostic(
					EntityIdentifier,
					Component.ScriptAsset,
					ScriptDiagnosticCode::LuaInitializationFailed,
					LuaErrorMessage(Instance->State)));
			}

			const auto AssetRecord = SourceProject.GetAssetRegistry().Find(Component.ScriptAsset);
			const std::string ChunkName = AssetRecord
				? "@" + AssetRecord->ProjectRelativePath.generic_string()
				: "@PulseForge script";
			const int LoadStatus = luaL_loadbufferx(
				Instance->State,
				ScriptSource.data(),
				ScriptSource.size(),
				ChunkName.c_str(),
				"t");
			if (LoadStatus != LUA_OK)
			{
				return std::unexpected(MakeDiagnostic(
					EntityIdentifier,
					Component.ScriptAsset,
					ScriptDiagnosticCode::CompileFailed,
					LuaErrorMessage(Instance->State)));
			}

			Instance->InstructionBudget =
				(Description.MaxInstructionsPerCallback / InstructionHookQuantum) * InstructionHookQuantum;
			lua_sethook(Instance->State, InstructionBudgetHook, LUA_MASKCOUNT, InstructionHookQuantum);
			const int ExecuteStatus = lua_pcall(Instance->State, 0, 0, 0);
			lua_sethook(Instance->State, nullptr, 0, 0);
			if (ExecuteStatus != LUA_OK)
			{
				return std::unexpected(MakeDiagnostic(
					EntityIdentifier,
					Component.ScriptAsset,
					ScriptDiagnosticCode::InitializationFailed,
					LuaErrorMessage(Instance->State)));
			}

			if (auto CreateResult = RunCallback(*Instance, 0); !CreateResult)
			{
				return std::unexpected(MakeDiagnostic(
					EntityIdentifier,
					Component.ScriptAsset,
					ScriptDiagnosticCode::InitializationFailed,
					std::move(CreateResult.error())));
			}
			return Instance;
		}

		void InvokeDestroy(ScriptInstance& Instance)
		{
			if (auto DestroyResult = RunCallback(Instance, 2); !DestroyResult)
			{
				RecordDiagnostic(MakeDiagnostic(
					Instance.BoundEntity.GetUUID(),
					Instance.Asset,
					ScriptDiagnosticCode::DestroyFailed,
					std::move(DestroyResult.error())));
			}
		}

		void Attach(Scene& SceneValue, const Entity& EntityValue, const ScriptComponent& Component)
		{
			try
			{
				auto Instance = CreateInstance(SceneValue, EntityValue, Component);
				if (!Instance)
				{
					RecordDiagnostic(std::move(Instance.error()));
					FailedAttachments.insert_or_assign(EntityValue.GetUUID(), Component.ScriptAsset);
					return;
				}
				Scripts.insert_or_assign(EntityValue.GetUUID(), std::move(*Instance));
				FailedAttachments.erase(EntityValue.GetUUID());
			}
			catch (const std::exception& Exception)
			{
				RecordDiagnostic(MakeDiagnostic(
					EntityValue.GetUUID(),
					Component.ScriptAsset,
					ScriptDiagnosticCode::LuaInitializationFailed,
					std::string("Could not initialize Lua script instance: ") + Exception.what()));
				FailedAttachments.insert_or_assign(EntityValue.GetUUID(), Component.ScriptAsset);
			}
		}

		void SynchronizeAttachments(Scene& SceneValue)
		{
			for (auto Iterator = Scripts.begin(); Iterator != Scripts.end();)
			{
				const auto CurrentEntity = SceneValue.FindEntity(Iterator->first);
				bool Remove = !CurrentEntity;
				if (CurrentEntity)
				{
					const auto Component = CurrentEntity->GetScript();
					if (!Component)
					{
						RecordDiagnostic(MakeDiagnostic(
							Iterator->first,
							Iterator->second->Asset,
							ScriptDiagnosticCode::InvalidComponent,
							Component.error().Message));
						Remove = true;
					}
					else
					{
						Remove = !Component->has_value() || !Component->value().Enabled ||
							Component->value().ScriptAsset != Iterator->second->Asset;
					}
				}
				if (!Remove)
				{
					++Iterator;
					continue;
				}

				InvokeDestroy(*Iterator->second);
				Iterator = Scripts.erase(Iterator);
			}

			for (auto Iterator = FailedAttachments.begin(); Iterator != FailedAttachments.end();)
			{
				const auto CurrentEntity = SceneValue.FindEntity(Iterator->first);
				const auto Component = CurrentEntity ? CurrentEntity->GetScript()
					: std::expected<std::optional<ScriptComponent>, SceneError>(std::optional<ScriptComponent>{});
				if (!Component || !Component->has_value() || !Component->value().Enabled ||
					Component->value().ScriptAsset != Iterator->second)
					Iterator = FailedAttachments.erase(Iterator);
				else
					++Iterator;
			}

			for (const Entity& EntityValue : SceneValue.GetEntities())
			{
				const auto Component = EntityValue.GetScript();
				if (!Component || !Component->has_value() || !Component->value().Enabled ||
					Scripts.contains(EntityValue.GetUUID()))
					continue;

				const auto Failed = FailedAttachments.find(EntityValue.GetUUID());
				if (Failed != FailedAttachments.end() && Failed->second == Component->value().ScriptAsset)
					continue;
				Attach(SceneValue, EntityValue, Component->value());
			}
		}
	};

	ScriptRuntime::ScriptRuntime(
		const Project& SourceProject,
		ScriptRuntimeDesc Description,
		ScriptRuntimeServices Services)
		: m_Project(SourceProject), m_Description(Description), m_Services(Services)
	{
	}

	ScriptRuntime::~ScriptRuntime()
	{
		Stop();
	}

	std::expected<void, ScriptRuntimeError> ScriptRuntime::Start(Scene& Source)
	{
		if (m_Impl)
			return std::unexpected(MakeRuntimeError(ScriptRuntimeErrorCode::AlreadyRunning, "Script runtime is already started"));
		if (m_Description.MaxSourceBytes == 0 || m_Description.MaxLuaMemoryBytes < 64u * 1024u ||
			m_Description.MaxTotalLuaMemoryBytes < 64u * 1024u ||
			m_Description.MaxInstructionsPerCallback < InstructionHookQuantum)
		{
			return std::unexpected(MakeRuntimeError(
				ScriptRuntimeErrorCode::InvalidSettings,
				"Script source, per-state/total memory, and instruction limits must be positive and large enough to initialize Lua"));
		}

		try
		{
			auto Candidate = std::make_unique<Impl>(m_Project, m_Description, m_Services);
			Candidate->Source = &Source;
			for (const Entity& EntityValue : Source.GetEntities())
			{
				const auto Component = EntityValue.GetScript();
				if (!Component)
				{
					Candidate->RecordDiagnostic(MakeDiagnostic(
						EntityValue.GetUUID(),
						{},
						ScriptDiagnosticCode::InvalidComponent,
						Component.error().Message));
					continue;
				}
				if (Component->has_value() && Component->value().Enabled)
					Candidate->Attach(Source, EntityValue, Component->value());
			}
			m_Impl = std::move(Candidate);
			m_StoppedDiagnostics.clear();
			return {};
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(MakeRuntimeError(
				ScriptRuntimeErrorCode::InitializationFailed,
				std::string("Could not start script runtime: ") + Exception.what()));
		}
	}

	void ScriptRuntime::Stop() noexcept
	{
		if (!m_Impl)
			return;
		try
		{
			for (auto& [Identifier, Instance] : m_Impl->Scripts)
			{
				(void)Identifier;
				m_Impl->InvokeDestroy(*Instance);
			}
		}
		catch (const std::exception& Exception)
		{
			PF_ERROR("Could not run Lua OnDestroy during shutdown: {0}", Exception.what());
		}
		m_StoppedDiagnostics = std::move(m_Impl->Diagnostics);
		m_Impl.reset();
	}

	bool ScriptRuntime::IsRunning() const noexcept
	{
		return m_Impl != nullptr;
	}

	size_t ScriptRuntime::GetScriptCount() const noexcept
	{
		return m_Impl ? m_Impl->Scripts.size() : 0;
	}

	std::span<const ScriptDiagnostic> ScriptRuntime::GetDiagnostics() const noexcept
	{
		return m_Impl
			? std::span<const ScriptDiagnostic>(m_Impl->Diagnostics)
			: std::span<const ScriptDiagnostic>(m_StoppedDiagnostics);
	}

	void ScriptRuntime::ClearDiagnostics() noexcept
	{
		if (m_Impl)
			m_Impl->Diagnostics.clear();
		m_StoppedDiagnostics.clear();
	}

	std::expected<void, ScriptRuntimeError> ScriptRuntime::Advance(Scene& Source, Timestep DeltaTime)
	{
		if (!m_Impl)
			return std::unexpected(MakeRuntimeError(ScriptRuntimeErrorCode::NotRunning, "Script runtime is not started"));
		if (m_Impl->Source != &Source)
			return std::unexpected(MakeRuntimeError(ScriptRuntimeErrorCode::DifferentScene, "Script runtime was started for a different Scene"));
		const double DeltaSeconds = DeltaTime.GetSeconds();
		if (!std::isfinite(DeltaSeconds) || DeltaSeconds < 0.0)
			return std::unexpected(MakeRuntimeError(ScriptRuntimeErrorCode::InvalidDeltaTime, "Script delta time must be finite and non-negative"));

		try
		{
			m_Impl->SynchronizeAttachments(Source);
			for (auto& [Identifier, Instance] : m_Impl->Scripts)
			{
				(void)Identifier;
				if (Instance->Faulted)
					continue;
				if (auto UpdateResult = RunCallback(*Instance, 1, DeltaSeconds); !UpdateResult)
				{
					Instance->Faulted = true;
					m_Impl->RecordDiagnostic(MakeDiagnostic(
						Instance->BoundEntity.GetUUID(),
						Instance->Asset,
						ScriptDiagnosticCode::UpdateFailed,
						std::move(UpdateResult.error())));
				}
			}
			return {};
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(MakeRuntimeError(
				ScriptRuntimeErrorCode::OperationFailed,
				std::string("Could not advance script runtime: ") + Exception.what()));
		}
	}
}
