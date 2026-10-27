#include "Scripting/ScriptEnvironment.h"
#include "Scripting/ScriptRuntimeInternal.h"

#if !defined(PF_COOKED_RUNTIME_ONLY)
#include "Assets/PrefabAssetService.h"
#include "Assets/Project.h"
#endif
#include "Runtime/CookedContentProvider.h"
#include "Audio/AudioSceneRuntime.h"
#include "Core/Input.h"
#include "Input/GameplayInput.h"
#include "Physics/PhysicsSceneRuntime.h"
#include "Scene/Scene.h"
#include "Scripting/Detail/ScriptFloatPropertyRead.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <limits>
#include <new>
#include <optional>
#include <string_view>
#include <utility>

extern "C"
{
#include <lualib.h>
}

namespace PulseForge::ScriptEnvironment
{
	using namespace ScriptRuntimeDetail;

	namespace
	{
		constexpr char EntityMetatableName[] = "PulseForge.Entity";

		struct LuaEntity final
		{
			Entity Value;
		};

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

		bool IsCurrentEntity(const ScriptInstance& Instance, const Entity& Value)
		{
			if (!Instance.SourceScene || !Value.IsValid()) return false;
			const auto Current = Instance.SourceScene->FindEntity(Value.GetUUID());
			return Current && *Current == Value;
		}

		int LuaEntityCollect(lua_State* State)
		{
			if (auto* UserData = GetLuaEntity(State)) std::destroy_at(UserData);
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

		template <size_t Capacity> void CopyHostError(std::array<char, Capacity>& Buffer, std::string_view Message)
		{
			const size_t CopyLength = std::min(Message.size(), Capacity - 1);
			std::copy_n(Message.data(), CopyLength, Buffer.data());
			Buffer[CopyLength] = '\0';
		}

		int LuaEntityUUID(lua_State* State)
		{
			LuaEntity* UserData = GetLuaEntity(State);
			if (!UserData) return ReturnHostError(State, "entity method requires an Entity value as self");

			constexpr char HexDigits[] = "0123456789abcdef";
			std::array<char, 36> Identifier{};
			const UUID Value = UserData->Value.GetUUID();
			size_t TextIndex = 0;
			for (size_t ByteIndex = 0; ByteIndex < 16; ++ByteIndex)
			{
				if (ByteIndex == 4 || ByteIndex == 6 || ByteIndex == 8 || ByteIndex == 10) Identifier[TextIndex++] = '-';
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
			if (!UserData) return ReturnHostError(State, "entity method requires an Entity value as self");

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
			if (!HasTransform) return ReturnHostError(State, "entity is no longer valid in its scene");

			lua_pushnumber(State, Translation.x);
			lua_pushnumber(State, Translation.y);
			lua_pushnumber(State, Translation.z);
			return 3;
		}

		int LuaEntityGetFloatProperty(lua_State* State)
		{
			LuaEntity* UserData = GetLuaEntity(State);
			if (!UserData) return ReturnHostError(State, "entity method requires an Entity value as self");
			if (lua_type(State, 2) != LUA_TSTRING) return ReturnHostError(State, "get_float_property requires a stable property ID string");

			size_t IdentifierLength = 0;
			const char* IdentifierText = lua_tolstring(State, 2, &IdentifierLength);
			const std::string_view Identifier(IdentifierText, IdentifierLength);
			if (!IsValidScriptPropertyIdentifier(Identifier))
				return ReturnHostError(State, "get_float_property received an invalid stable property ID");

			const auto Result = CaptureScriptFloatPropertyRead(
				[&UserData, Identifier] { return ReadProperty(UserData->Value, PropertyComponentIds::ScriptFloat, Identifier); });
			if (Result.State == ScriptFloatPropertyReadState::Failed) return ReturnHostError(State, Result.Failure.data());
			if (Result.State == ScriptFloatPropertyReadState::Missing)
			{
				lua_pushnil(State);
				return 1;
			}
			lua_pushnumber(State, Result.Value);
			return 1;
		}

		int LuaEntitySetTranslation(lua_State* State)
		{
			LuaEntity* UserData = GetLuaEntity(State);
			if (!UserData) return ReturnHostError(State, "entity method requires an Entity value as self");

			int IsXNumber = 0;
			int IsYNumber = 0;
			int IsZNumber = 0;
			const lua_Number X = lua_tonumberx(State, 2, &IsXNumber);
			const lua_Number Y = lua_tonumberx(State, 3, &IsYNumber);
			const lua_Number Z = lua_tonumberx(State, 4, &IsZNumber);
			if (!IsXNumber || !IsYNumber || !IsZNumber || !std::isfinite(X) || !std::isfinite(Y) || !std::isfinite(Z))
				return ReturnHostError(State, "set_translation requires three finite numbers");

			const char* Failure = nullptr;
			{
				auto Transform = UserData->Value.GetTransform();
				if (!Transform) Failure = "entity is no longer valid in its scene";
				else
				{
					Transform->Translation = { static_cast<float>(X), static_cast<float>(Y), static_cast<float>(Z) };
					if (!std::isfinite(Transform->Translation.x) || !std::isfinite(Transform->Translation.y) ||
						!std::isfinite(Transform->Translation.z))
						Failure = "translation values are outside the supported range";
					else if (!UserData->Value.SetTransform(*Transform)) Failure = "scene rejected the entity transform";
				}
			}
			if (Failure) return ReturnHostError(State, Failure);
			lua_pushboolean(State, true);
			return 1;
		}

		int LuaEntityApplyForce(lua_State* State)
		{
			LuaEntity* UserData = GetLuaEntity(State);
			if (!UserData) return ReturnHostError(State, "entity method requires an Entity value as self");
			ScriptInstance* Instance = GetScriptInstance(State);
			if (!Instance || !Instance->Services.Physics)
				return ReturnHostError(State, "physics service is unavailable to this script runtime");

			int IsXNumber = 0;
			int IsYNumber = 0;
			int IsZNumber = 0;
			const lua_Number X = lua_tonumberx(State, 2, &IsXNumber);
			const lua_Number Y = lua_tonumberx(State, 3, &IsYNumber);
			const lua_Number Z = lua_tonumberx(State, 4, &IsZNumber);
			if (!IsXNumber || !IsYNumber || !IsZNumber || !std::isfinite(X) || !std::isfinite(Y) || !std::isfinite(Z))
				return ReturnHostError(State, "apply_force requires three finite numbers");

			const glm::vec3 Force{ static_cast<float>(X), static_cast<float>(Y), static_cast<float>(Z) };
			if (!std::isfinite(Force.x) || !std::isfinite(Force.y) || !std::isfinite(Force.z))
				return ReturnHostError(State, "force values are outside the supported range");
			if (!IsCurrentEntity(*Instance, UserData->Value)) return ReturnHostError(State, "entity is no longer valid in its scene");

			std::array<char, 512> Failure{};
			bool Applied = false;
			try
			{
				{
					auto Result = Instance->Services.Physics->ApplyForce(UserData->Value.GetUUID(), Force);
					if (Result) Applied = true;
					else CopyHostError(Failure, Result.error().Message);
				}
			}
			catch (const std::exception& Exception)
			{
				CopyHostError(Failure, Exception.what());
			}
			catch (...)
			{
				CopyHostError(Failure, "Physics service failed while applying force");
			}
			if (!Applied) return ReturnHostError(State, Failure.data());
			lua_pushboolean(State, true);
			return 1;
		}

		using AudioControl = std::expected<void, AudioSceneRuntimeError> (AudioSceneRuntime::*)(UUID);

		int InvokeAudioControl(lua_State* State, AudioControl Control)
		{
			LuaEntity* UserData = GetLuaEntity(State);
			if (!UserData) return ReturnHostError(State, "entity method requires an Entity value as self");
			ScriptInstance* Instance = GetScriptInstance(State);
			if (!Instance || !Instance->Services.Audio)
				return ReturnHostError(State, "audio service is unavailable to this script runtime");
			if (!IsCurrentEntity(*Instance, UserData->Value)) return ReturnHostError(State, "entity is no longer valid in its scene");

			std::array<char, 512> Failure{};
			bool Succeeded = false;
			try
			{
				{
					auto Result = (Instance->Services.Audio->*Control)(UserData->Value.GetUUID());
					if (Result) Succeeded = true;
					else CopyHostError(Failure, Result.error().Message);
				}
			}
			catch (const std::exception& Exception)
			{
				CopyHostError(Failure, Exception.what());
			}
			catch (...)
			{
				CopyHostError(Failure, "Audio service failed while controlling playback");
			}
			if (!Succeeded) return ReturnHostError(State, Failure.data());
			lua_pushboolean(State, true);
			return 1;
		}

		bool ReadIntegerArgument(lua_State* State, int Index, int& Value)
		{
			int IsInteger = 0;
			const lua_Integer LuaValue = lua_tointegerx(State, Index, &IsInteger);
			if (!IsInteger || LuaValue < std::numeric_limits<int>::min() || LuaValue > std::numeric_limits<int>::max()) return false;
			Value = static_cast<int>(LuaValue);
			return true;
		}

		int LuaInputIsKeyPressed(lua_State* State)
		{
			ScriptInstance* Instance = GetScriptInstance(State);
			if (!Instance || !Instance->Services.InputState)
				return ReturnHostError(State, "input service is unavailable to this script runtime");

			int KeyCode = 0;
			if (!ReadIntegerArgument(State, 1, KeyCode)) return ReturnHostError(State, "input.is_key_pressed requires an integer key code");
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

		bool ReadActionIdentifier(lua_State* State, std::string_view& Identifier)
		{
			if (lua_type(State, 1) != LUA_TSTRING) return false;
			size_t Length = 0;
			const char* Data = lua_tolstring(State, 1, &Length);
			if (!Data || Length == 0) return false;
			Identifier = std::string_view(Data, Length);
			return true;
		}

		const ActionValue* FindLuaAction(lua_State* State)
		{
			ScriptInstance* Instance = GetScriptInstance(State);
			if (!Instance || !Instance->Services.GameplayInput)
			{
				ReturnHostError(State, "gameplay input service is unavailable to this script runtime");
				return nullptr;
			}

			std::string_view Identifier;
			if (!ReadActionIdentifier(State, Identifier))
			{
				ReturnHostError(State, "gameplay action queries require a non-empty string action id");
				return nullptr;
			}

			const ActionValue* Value = Instance->Services.GameplayInput->FindAction(Identifier);
			if (!Value) ReturnHostError(State, "unknown gameplay action id");
			return Value;
		}

		int LuaInputIsActionHeld(lua_State* State)
		{
			const ActionValue* Value = FindLuaAction(State);
			if (!Value) return 2;
			if (Value->Type != ActionValueType::Digital)
				return ReturnHostError(State, "input.is_action_held requires a digital action");
			lua_pushboolean(State, Value->Held);
			return 1;
		}

		int LuaInputWasActionPressed(lua_State* State)
		{
			const ActionValue* Value = FindLuaAction(State);
			if (!Value) return 2;
			if (Value->Type != ActionValueType::Digital)
				return ReturnHostError(State, "input.was_action_pressed requires a digital action");
			lua_pushboolean(State, Value->Pressed);
			return 1;
		}

		int LuaInputWasActionReleased(lua_State* State)
		{
			const ActionValue* Value = FindLuaAction(State);
			if (!Value) return 2;
			if (Value->Type != ActionValueType::Digital)
				return ReturnHostError(State, "input.was_action_released requires a digital action");
			lua_pushboolean(State, Value->Released);
			return 1;
		}

		int LuaInputGetAxis(lua_State* State)
		{
			const ActionValue* Value = FindLuaAction(State);
			if (!Value) return 2;
			if (Value->Type != ActionValueType::Axis1D)
				return ReturnHostError(State, "input.get_axis requires a one-dimensional axis action");
			lua_pushnumber(State, Value->X);
			return 1;
		}

		int LuaInputGetVector(lua_State* State)
		{
			const ActionValue* Value = FindLuaAction(State);
			if (!Value) return 2;
			if (Value->Type != ActionValueType::Axis2D)
				return ReturnHostError(State, "input.get_vector requires a two-dimensional axis action");
			lua_pushnumber(State, Value->X);
			lua_pushnumber(State, Value->Y);
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
			if (!UserData) return ReturnHostError(State, "entity method requires an Entity value as self");
			ScriptInstance* Instance = GetScriptInstance(State);
			if (!Instance || !Instance->SourceScene) return ReturnHostError(State, "scene service is no longer available");

			std::array<char, 512> Failure{};
			bool Destroyed = false;
			try
			{
				{
					auto Result = Instance->SourceScene->DestroyEntity(UserData->Value);
					if (Result) Destroyed = true;
					else CopyHostError(Failure, Result.error().Message);
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
			if (!Destroyed) return ReturnHostError(State, Failure.data());
			lua_pushboolean(State, true);
			return 1;
		}

		std::optional<UUID> ReadUUIDArgument(lua_State* State, int Index)
		{
			if (lua_type(State, Index) != LUA_TSTRING) return std::nullopt;
			size_t Length = 0;
			const char* Text = lua_tolstring(State, Index, &Length);
			const auto Parsed = UUID::Parse(std::string_view(Text, Length));
			if (!Parsed) return std::nullopt;
			return *Parsed;
		}

		int LuaSceneFindEntity(lua_State* State)
		{
			ScriptInstance* Instance = GetScriptInstance(State);
			if (!Instance || !Instance->SourceScene) return ReturnHostError(State, "scene service is no longer available");
			const auto Identifier = ReadUUIDArgument(State, 1);
			if (!Identifier) return ReturnHostError(State, "scene.find_entity requires a canonical UUID");

			auto* UserData = CreateLuaEntityUserData(State);
			bool Found = false;
			{
				const auto FoundEntity = Instance->SourceScene->FindEntity(*Identifier);
				if (FoundEntity &&
					!(Instance->Destroying && *Identifier == Instance->BoundEntity.GetUUID() && *FoundEntity != Instance->BoundEntity))
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
			if (!Instance || !Instance->SourceScene) return ReturnHostError(State, "scene service is no longer available");
			if (lua_type(State, 1) != LUA_TSTRING) return ReturnHostError(State, "scene.create_entity requires a name string");

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
					else CopyHostError(Failure, Result.error().Message);
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
			if (!Instance || !Instance->SourceScene || (!Instance->SourceProject && !Instance->CookedAssets))
				return ReturnHostError(State, "scene or asset provider is no longer available");
			const auto PrefabIdentifier = ReadUUIDArgument(State, 1);
			if (!PrefabIdentifier) return ReturnHostError(State, "scene.spawn_prefab requires a canonical prefab asset UUID");

			auto* UserData = CreateLuaEntityUserData(State);
			std::array<char, 512> Failure{};
			bool Spawned = false;
			try
			{
				{
#if defined(PF_COOKED_RUNTIME_ONLY)
					auto Result = Instance->CookedAssets->InstantiatePrefab(*PrefabIdentifier, *Instance->SourceScene);
#else
					auto Result = Instance->CookedAssets
						? Instance->CookedAssets->InstantiatePrefab(*PrefabIdentifier, *Instance->SourceScene)
						: [&]() -> std::expected<Entity, CookedAssetCatalogError>
						{
							auto SourceResult = PrefabAssetService::Instantiate(*PrefabIdentifier, Instance->SourceProject->GetRootPath(),
								Instance->SourceProject->GetAssetRegistry(), *Instance->SourceScene);
							if (!SourceResult)
								return std::unexpected(CookedAssetCatalogError{ .Code = CookedAssetCatalogErrorCode::InvalidPayload,
									.Asset = *PrefabIdentifier, .Message = SourceResult.error().Message });
							return *SourceResult;
						}();
#endif
					if (Result)
					{
						UserData->Value = std::move(*Result);
						Spawned = true;
					}
					else CopyHostError(Failure, Result.error().Message);
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

	} // namespace

	int OpenRestrictedLibraries(lua_State* State)
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

	int Initialize(lua_State* State)
	{
		ScriptInstance* Instance = GetScriptInstance(State);
		if (!Instance || !Instance->SourceScene) return 0;

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
		lua_pushcfunction(State, LuaEntityGetFloatProperty);
		lua_setfield(State, -2, "get_float_property");
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
		lua_pushcfunction(State, LuaInputIsActionHeld);
		lua_setfield(State, -2, "is_action_held");
		lua_pushcfunction(State, LuaInputWasActionPressed);
		lua_setfield(State, -2, "was_action_pressed");
		lua_pushcfunction(State, LuaInputWasActionReleased);
		lua_setfield(State, -2, "was_action_released");
		lua_pushcfunction(State, LuaInputGetAxis);
		lua_setfield(State, -2, "get_axis");
		lua_pushcfunction(State, LuaInputGetVector);
		lua_setfield(State, -2, "get_vector");
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

} // namespace PulseForge::ScriptEnvironment
