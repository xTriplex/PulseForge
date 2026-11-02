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
#include "UI/RuntimeUI.h"

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

		template <size_t Capacity> void CopyHostError(std::array<char, Capacity>& Buffer, std::string_view Message);
		void RecordModuleError(ScriptInstance& Instance, std::string_view Message) noexcept;

		ScriptModuleRuntime* FindScriptModule(ScriptInstance& Instance, std::string_view ProjectRelativePath)
		{
			const auto It = std::lower_bound(Instance.Modules.begin(), Instance.Modules.end(),
				ProjectRelativePath, [](const ScriptModuleRuntime& Module, std::string_view Path)
				{ return Module.ProjectRelativePath < Path; });
			return It != Instance.Modules.end() && It->ProjectRelativePath == ProjectRelativePath
				? &*It : nullptr;
		}

		int LuaRequire(lua_State* State)
		{
			ScriptInstance* Instance = GetScriptInstance(State);
			if (!Instance || !Instance->SourceSnapshot)
				return ReturnHostError(State, "script module snapshot is unavailable");
			if (lua_type(State, 1) != LUA_TSTRING)
			{
				RecordModuleError(*Instance, "require expects a declared module path string");
				return ReturnHostError(State, "require expects a declared module path string");
			}

			size_t RequestedLength = 0;
			const char* RequestedText = lua_tolstring(State, 1, &RequestedLength);
			const std::string_view RequestedName(RequestedText, RequestedLength);
			size_t ImporterLength = 0;
			const char* ImporterText = lua_tolstring(State, lua_upvalueindex(1), &ImporterLength);
			if (!ImporterText)
			{
				RecordModuleError(*Instance, "require has no declared importer identity");
				return ReturnHostError(State, "require has no declared importer identity");
			}
			const std::string_view ImporterPath(ImporterText, ImporterLength);

			const std::vector<ScriptModuleImport>* Imports = nullptr;
			if (ImporterPath == Instance->SourceSnapshot->MainSourcePath)
				Imports = &Instance->SourceSnapshot->MainImports;
			else if (ScriptModuleRuntime* Importer = FindScriptModule(*Instance, ImporterPath))
				Imports = &Importer->Imports;
			if (!Imports)
			{
				RecordModuleError(*Instance, "require importer is not part of the validated script snapshot");
				return ReturnHostError(State, "require importer is not part of the validated script snapshot");
			}

			const auto Import = std::find_if(Imports->begin(), Imports->end(), [RequestedName](const ScriptModuleImport& Candidate)
			{ return Candidate.DeclaredName == RequestedName; });
			if (Import == Imports->end())
			{
				RecordModuleError(*Instance, "script attempted to load a module it did not declare");
				return ReturnHostError(State, "script attempted to load a module it did not declare");
			}

			ScriptModuleRuntime* Module = FindScriptModule(*Instance, Import->TargetPath);
			if (!Module)
			{
				RecordModuleError(*Instance, "declared script module is missing from the validated snapshot");
				return ReturnHostError(State, "declared script module is missing from the validated snapshot");
			}
			if (Module->State == ScriptModuleLoadState::Loaded)
			{
				lua_rawgeti(State, LUA_REGISTRYINDEX, Module->ValueReference);
				return 1;
			}
			if (Module->State == ScriptModuleLoadState::Loading)
			{
				RecordModuleError(*Instance, "cyclic script module load was rejected");
				return ReturnHostError(State, "cyclic script module load was rejected");
			}

			Module->State = ScriptModuleLoadState::Loading;
			lua_rawgeti(State, LUA_REGISTRYINDEX, Module->CompiledChunkReference);
			PushChunkEnvironment(State, Module->ProjectRelativePath);
			(void)lua_setupvalue(State, -2, 1);
			const int Status = lua_pcall(State, 0, 1, 0);
			if (Status != LUA_OK)
			{
				std::array<char, 512> Error{};
				size_t ErrorLength = 0;
				const char* ErrorText = lua_tolstring(State, -1, &ErrorLength);
				if (!ErrorText) ErrorText = "script module evaluation failed";
				const std::string_view ErrorView(ErrorText,
					ErrorLength == 0 ? std::char_traits<char>::length(ErrorText) : ErrorLength);
				CopyHostError(Error, ErrorView);
				RecordModuleError(*Instance, ErrorView);
				lua_pop(State, 1);
				Module->State = ScriptModuleLoadState::Unloaded;
				lua_pushnil(State);
				lua_pushstring(State, Error.data());
				return 2;
			}

			if (lua_isnil(State, -1))
			{
				lua_pop(State, 1);
				lua_pushboolean(State, 1);
			}
			lua_pushvalue(State, -1);
			Module->ValueReference = luaL_ref(State, LUA_REGISTRYINDEX);
			Module->State = ScriptModuleLoadState::Loaded;
			return 1;
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

		bool ReadFiniteFloat(lua_State* State, int Index, float& Value)
		{
			int IsNumber = 0;
			const lua_Number Number = lua_tonumberx(State, Index, &IsNumber);
			if (!IsNumber || !std::isfinite(Number)) return false;
			Value = static_cast<float>(Number);
			return std::isfinite(Value);
		}

		bool ReadUInt32(lua_State* State, int Index, uint32_t& Value)
		{
			int IsInteger = 0;
			const lua_Integer Number = lua_tointegerx(State, Index, &IsInteger);
			if (!IsInteger || Number < 0 || static_cast<uint64_t>(Number) > std::numeric_limits<uint32_t>::max()) return false;
			Value = static_cast<uint32_t>(Number);
			return true;
		}

		bool ReadOptionalQueryFilter(lua_State* State, int MaskIndex, PhysicsQueryFilter& Filter)
		{
			if (!lua_isnoneornil(State, MaskIndex) && !ReadUInt32(State, MaskIndex, Filter.CategoryMask)) return false;
			const int SensorIndex = MaskIndex + 1;
			if (!lua_isnoneornil(State, SensorIndex))
			{
				if (!lua_isboolean(State, SensorIndex)) return false;
				Filter.IncludeSensors = lua_toboolean(State, SensorIndex) != 0;
			}
			return true;
		}

		void PushEntityUserData(lua_State* State, const Entity& EntityValue)
		{
			auto* UserData = static_cast<LuaEntity*>(lua_newuserdatauv(State, sizeof(LuaEntity), 0));
			new (UserData) LuaEntity{ EntityValue };
			luaL_getmetatable(State, EntityMetatableName);
			lua_setmetatable(State, -2);
		}

		void PushVectorTable(lua_State* State, const glm::vec3& Value)
		{
			lua_createtable(State, 0, 3);
			lua_pushnumber(State, Value.x);
			lua_setfield(State, -2, "x");
			lua_pushnumber(State, Value.y);
			lua_setfield(State, -2, "y");
			lua_pushnumber(State, Value.z);
			lua_setfield(State, -2, "z");
		}

		void PushQueryHitTable(lua_State* State, const PhysicsQueryHit& Hit)
		{
			lua_createtable(State, 0, 6);
			PushEntityUserData(State, Hit.Target);
			lua_setfield(State, -2, "entity");
			constexpr char Hex[] = "0123456789abcdef";
			std::array<char, 36> Identifier{};
			size_t TextIndex = 0;
			for (size_t ByteIndex = 0; ByteIndex < 16; ++ByteIndex)
			{
				if (ByteIndex == 4 || ByteIndex == 6 || ByteIndex == 8 || ByteIndex == 10) Identifier[TextIndex++] = '-';
				const uint64_t Word = ByteIndex < 8 ? Hit.EntityIdentifier.GetHigh() : Hit.EntityIdentifier.GetLow();
				const size_t WordByteIndex = ByteIndex % 8;
				const uint8_t Byte = static_cast<uint8_t>((Word >> ((7 - WordByteIndex) * 8)) & 0xff);
				Identifier[TextIndex++] = Hex[Byte >> 4];
				Identifier[TextIndex++] = Hex[Byte & 0xf];
			}
			lua_pushlstring(State, Identifier.data(), Identifier.size());
			lua_setfield(State, -2, "uuid");
			lua_pushnumber(State, Hit.Fraction);
			lua_setfield(State, -2, "fraction");
			PushVectorTable(State, Hit.Position);
			lua_setfield(State, -2, "position");
			PushVectorTable(State, Hit.Normal);
			lua_setfield(State, -2, "normal");
			lua_pushinteger(State, static_cast<lua_Integer>(Hit.SessionGeneration));
			lua_setfield(State, -2, "session_generation");
			lua_pushinteger(State, static_cast<lua_Integer>(Hit.BodyGeneration));
			lua_setfield(State, -2, "body_generation");
		}

		int LuaPhysicsRaycast(lua_State* State)
		{
			ScriptInstance* Instance = GetScriptInstance(State);
			if (!Instance || !Instance->Services.Physics)
				return ReturnHostError(State, "physics query service is unavailable to this runtime");
			glm::vec3 Origin{};
			glm::vec3 Direction{};
			float MaxDistance = 0.0f;
			if (!ReadFiniteFloat(State, 1, Origin.x) || !ReadFiniteFloat(State, 2, Origin.y) ||
				!ReadFiniteFloat(State, 3, Origin.z) || !ReadFiniteFloat(State, 4, Direction.x) ||
				!ReadFiniteFloat(State, 5, Direction.y) || !ReadFiniteFloat(State, 6, Direction.z) ||
				!ReadFiniteFloat(State, 7, MaxDistance))
				return ReturnHostError(State, "physics.raycast expects finite origin, direction, and distance numbers");
			UUID Excluded = Instance->BoundEntity.GetUUID();
			PhysicsQueryFilter Filter;
			Filter.ExcludedEntities = std::span<const UUID>(&Excluded, 1);
			if (!ReadOptionalQueryFilter(State, 8, Filter))
				return ReturnHostError(State, "physics.raycast filter expects an optional uint32 mask and boolean sensor flag");

			Instance->HostOperationError.fill('\0');
			bool Succeeded = false;
			{
				try
				{
					auto Result = Instance->Services.Physics->Raycast(Origin, Direction, MaxDistance, Filter);
					if (!Result) CopyHostError(Instance->HostOperationError, Result.error().Message);
					else
					{
						Instance->QueryHitScratch = std::move(*Result);
						Succeeded = true;
					}
				}
				catch (const std::exception& Exception) { CopyHostError(Instance->HostOperationError, Exception.what()); }
				catch (...) { CopyHostError(Instance->HostOperationError, "Physics ray query failed unexpectedly"); }
			}
			if (!Succeeded) return ReturnHostError(State, Instance->HostOperationError.data());
			if (!Instance->QueryHitScratch || !Instance->QueryHitScratch->Target.IsValid())
			{
				lua_pushnil(State);
				return 1;
			}
			PushQueryHitTable(State, *Instance->QueryHitScratch);
			return 1;
		}

		int LuaPhysicsShapeQuery(lua_State* State, ColliderShape Shape, bool Cast)
		{
			ScriptInstance* Instance = GetScriptInstance(State);
			if (!Instance || !Instance->Services.Physics)
				return ReturnHostError(State, "physics query service is unavailable to this runtime");
			BoxColliderComponent QueryShape;
			QueryShape.Shape = Shape;
			int PositionIndex = 0;
			if (Shape == ColliderShape::Box)
			{
				if (!ReadFiniteFloat(State, 1, QueryShape.HalfExtents.x) || !ReadFiniteFloat(State, 2, QueryShape.HalfExtents.y) ||
					!ReadFiniteFloat(State, 3, QueryShape.HalfExtents.z))
					return ReturnHostError(State, "box query requires three finite half-extents");
				PositionIndex = 4;
			}
			else if (Shape == ColliderShape::Sphere)
			{
				if (!ReadFiniteFloat(State, 1, QueryShape.SphereRadius))
					return ReturnHostError(State, "sphere query requires a finite radius");
				PositionIndex = 2;
			}
			else
			{
				if (!ReadFiniteFloat(State, 1, QueryShape.CapsuleRadius) || !ReadFiniteFloat(State, 2, QueryShape.CapsuleHeight))
					return ReturnHostError(State, "capsule query requires finite radius and total height");
				PositionIndex = 3;
			}
			glm::vec3 Position{};
			if (!ReadFiniteFloat(State, PositionIndex, Position.x) || !ReadFiniteFloat(State, PositionIndex + 1, Position.y) ||
				!ReadFiniteFloat(State, PositionIndex + 2, Position.z))
				return ReturnHostError(State, "shape query requires a finite world position");
			int NextIndex = PositionIndex + 3;
			glm::vec3 Direction{};
			float MaxDistance = 0.0f;
			uint32_t MaxResults = 16;
			if (Cast)
			{
				if (!ReadFiniteFloat(State, NextIndex, Direction.x) || !ReadFiniteFloat(State, NextIndex + 1, Direction.y) ||
					!ReadFiniteFloat(State, NextIndex + 2, Direction.z) || !ReadFiniteFloat(State, NextIndex + 3, MaxDistance))
					return ReturnHostError(State, "shape cast requires a finite direction and positive distance");
				NextIndex += 4;
			}
			else if (!lua_isnoneornil(State, NextIndex))
			{
				if (!ReadUInt32(State, NextIndex, MaxResults) || MaxResults == 0 || MaxResults > 64)
					return ReturnHostError(State, "shape overlap result limit must be between 1 and 64");
				++NextIndex;
			}
			UUID Excluded = Instance->BoundEntity.GetUUID();
			PhysicsQueryFilter Filter;
			Filter.ExcludedEntities = std::span<const UUID>(&Excluded, 1);
			if (!ReadOptionalQueryFilter(State, NextIndex, Filter))
				return ReturnHostError(State, "shape query filter expects an optional uint32 mask and boolean sensor flag");

			Instance->HostOperationError.fill('\0');
			bool Succeeded = false;
			{
				try
				{
					const glm::quat Rotation(1.0f, 0.0f, 0.0f, 0.0f);
					if (Cast)
					{
						auto Result = Instance->Services.Physics->CastShape(QueryShape, Position, Rotation, Direction, MaxDistance, Filter);
						if (!Result) CopyHostError(Instance->HostOperationError, Result.error().Message);
						else { Instance->QueryHitScratch = std::move(*Result); Succeeded = true; }
					}
					else
				{
					auto Result = Instance->Services.Physics->OverlapShape(QueryShape, Position, Rotation, Filter, MaxResults);
					if (!Result) CopyHostError(Instance->HostOperationError, Result.error().Message);
					else { Instance->QueryOverlapScratch = std::move(*Result); Succeeded = true; }
				}
				}
				catch (const std::exception& Exception) { CopyHostError(Instance->HostOperationError, Exception.what()); }
				catch (...) { CopyHostError(Instance->HostOperationError, "Physics shape query failed unexpectedly"); }
			}
			if (!Succeeded) return ReturnHostError(State, Instance->HostOperationError.data());
			if (Cast)
			{
				if (!Instance->QueryHitScratch || !Instance->QueryHitScratch->Target.IsValid()) lua_pushnil(State);
				else PushQueryHitTable(State, *Instance->QueryHitScratch);
				return 1;
			}
			lua_createtable(State, 0, 2);
			lua_createtable(State, static_cast<int>(Instance->QueryOverlapScratch.Hits.size()), 0);
			lua_Integer ResultIndex = 1;
			for (const PhysicsQueryHit& Hit : Instance->QueryOverlapScratch.Hits)
			{
				if (!Hit.Target.IsValid()) continue;
				PushQueryHitTable(State, Hit);
				lua_rawseti(State, -2, ResultIndex++);
			}
			lua_setfield(State, -2, "hits");
			lua_pushboolean(State, Instance->QueryOverlapScratch.Truncated);
			lua_setfield(State, -2, "truncated");
			return 1;
		}

		int LuaPhysicsOverlapBox(lua_State* State) { return LuaPhysicsShapeQuery(State, ColliderShape::Box, false); }
		int LuaPhysicsOverlapSphere(lua_State* State) { return LuaPhysicsShapeQuery(State, ColliderShape::Sphere, false); }
		int LuaPhysicsOverlapCapsule(lua_State* State) { return LuaPhysicsShapeQuery(State, ColliderShape::Capsule, false); }
		int LuaPhysicsCastBox(lua_State* State) { return LuaPhysicsShapeQuery(State, ColliderShape::Box, true); }
		int LuaPhysicsCastSphere(lua_State* State) { return LuaPhysicsShapeQuery(State, ColliderShape::Sphere, true); }
		int LuaPhysicsCastCapsule(lua_State* State) { return LuaPhysicsShapeQuery(State, ColliderShape::Capsule, true); }

		template <size_t Capacity> void CopyHostError(std::array<char, Capacity>& Buffer, std::string_view Message)
		{
			const size_t CopyLength = std::min(Message.size(), Capacity - 1);
			std::copy_n(Message.data(), CopyLength, Buffer.data());
			Buffer[CopyLength] = '\0';
		}

		void RecordModuleError(ScriptInstance& Instance, std::string_view Message) noexcept
		{
			CopyHostError(Instance.ModuleFailureMessage, Message);
			Instance.ModuleFailurePending = true;
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
			ScriptInstance* Instance = GetScriptInstance(State);
			const ScriptFloatPropertyDeclaration* Declaration = nullptr;
			if (Instance && Instance->SourceSnapshot)
			{
				const auto It = std::find_if(Instance->SourceSnapshot->Metadata.FloatProperties.begin(),
					Instance->SourceSnapshot->Metadata.FloatProperties.end(), [Identifier](const ScriptFloatPropertyDeclaration& Value)
					{ return Value.ID == Identifier; });
				if (It != Instance->SourceSnapshot->Metadata.FloatProperties.end()) Declaration = &*It;
			}
			if (Result.State == ScriptFloatPropertyReadState::Missing)
			{
				if (Declaration)
				{
					lua_pushnumber(State, Declaration->DefaultValue);
					return 1;
				}
				lua_pushnil(State);
				return 1;
			}
			if (Declaration && (Result.Value < Declaration->Minimum || Result.Value > Declaration->Maximum))
				return ReturnHostError(State, "authored script property override is outside its declared range");
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

		int LuaSceneRequestTransition(lua_State* State)
		{
			ScriptInstance* Instance = GetScriptInstance(State);
			if (!Instance || lua_type(State, 1) != LUA_TSTRING)
				return ReturnHostError(State, "scene.request_transition requires a scene asset UUID string");
			size_t Length = 0;
			const char* Text = lua_tolstring(State, 1, &Length);
			const auto Identifier = AssetID::Parse(std::string_view(Text, Length));
			if (!Identifier) return ReturnHostError(State, "scene.request_transition requires a canonical scene asset UUID");
			if (!Instance->Services.QueueSceneTransition)
				return ReturnHostError(State, "scene transitions are unavailable in this runtime host");
			bool Queued = false;
			try { Queued = Instance->Services.QueueSceneTransition(*Identifier); }
			catch (...) { Queued = false; }
			if (!Queued) return ReturnHostError(State, "the runtime host rejected the scene transition request");
			Instance->DeferredRuntimeTransitionRequested = true;
			lua_pushboolean(State, true);
			return 1;
		}

		int LuaSceneRequestStop(lua_State* State)
		{
			ScriptInstance* Instance = GetScriptInstance(State);
			if (!Instance || !Instance->Services.QueueStop)
				return ReturnHostError(State, "runtime stop requests are unavailable in this runtime host");
			bool Queued = false;
			try { Queued = Instance->Services.QueueStop(); }
			catch (...) { Queued = false; }
			if (!Queued) return ReturnHostError(State, "the runtime host rejected the stop request");
			Instance->DeferredRuntimeTransitionRequested = true;
			lua_pushboolean(State, true);
			return 1;
		}

		int LuaSceneRequestPause(lua_State* State)
		{
			ScriptInstance* Instance = GetScriptInstance(State);
			if (!Instance || lua_type(State, 1) != LUA_TBOOLEAN)
				return ReturnHostError(State, "scene.request_pause requires a boolean paused value");
			if (!Instance->Services.QueuePause)
				return ReturnHostError(State, "runtime pause requests are unavailable in this runtime host");
			const bool Paused = lua_toboolean(State, 1) != 0;
			bool Queued = false;
			try { Queued = Instance->Services.QueuePause(Paused); }
			catch (...) { Queued = false; }
			if (!Queued) return ReturnHostError(State, "the runtime host rejected the pause request");
			lua_pushboolean(State, true);
			return 1;
		}

		int LuaUISetText(lua_State* State)
		{
			ScriptInstance* Instance = GetScriptInstance(State);
			if (!Instance || !Instance->Services.SetRuntimeUIText || lua_type(State, 1) != LUA_TSTRING ||
				lua_type(State, 2) != LUA_TSTRING)
			{
				lua_pushboolean(State, false);
				return 1;
			}
			size_t IdentifierBytes = 0;
			size_t TextBytes = 0;
			const char* IdentifierText = lua_tolstring(State, 1, &IdentifierBytes);
			const char* Text = lua_tolstring(State, 2, &TextBytes);
			if (TextBytes > RuntimeUIDocument::MaximumTextBytes)
			{
				lua_pushboolean(State, false);
				return 1;
			}
			const auto Identifier = UUID::Parse(std::string_view(IdentifierText, IdentifierBytes));
			if (!Identifier)
			{
				lua_pushboolean(State, false);
				return 1;
			}
			bool Updated = false;
			try { Updated = Instance->Services.SetRuntimeUIText(*Identifier, std::string(Text, TextBytes)); }
			catch (...) { Updated = false; }
			lua_pushboolean(State, Updated);
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
		lua_pushcfunction(State, LuaSceneRequestTransition);
		lua_setfield(State, -2, "request_transition");
		lua_pushcfunction(State, LuaSceneRequestStop);
		lua_setfield(State, -2, "request_stop");
		lua_pushcfunction(State, LuaSceneRequestPause);
		lua_setfield(State, -2, "request_pause");
		lua_setglobal(State, "scene");

		lua_newtable(State);
		lua_pushcfunction(State, LuaUISetText);
		lua_setfield(State, -2, "set_text");
		lua_setglobal(State, "ui");

		lua_newtable(State);
		lua_pushcfunction(State, LuaPhysicsRaycast);
		lua_setfield(State, -2, "raycast");
		lua_pushcfunction(State, LuaPhysicsOverlapBox);
		lua_setfield(State, -2, "overlap_box");
		lua_pushcfunction(State, LuaPhysicsOverlapSphere);
		lua_setfield(State, -2, "overlap_sphere");
		lua_pushcfunction(State, LuaPhysicsOverlapCapsule);
		lua_setfield(State, -2, "overlap_capsule");
		lua_pushcfunction(State, LuaPhysicsCastBox);
		lua_setfield(State, -2, "cast_box");
		lua_pushcfunction(State, LuaPhysicsCastSphere);
		lua_setfield(State, -2, "cast_sphere");
		lua_pushcfunction(State, LuaPhysicsCastCapsule);
		lua_setfield(State, -2, "cast_capsule");
		lua_setglobal(State, "physics");

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
		lua_pushcfunction(State, ExecutePreparedChunk);
		Instance->ExecuteChunkCallbackReference = luaL_ref(State, LUA_REGISTRYINDEX);
		lua_pushcfunction(State, DispatchContactCallback);
		Instance->ContactCallbackDispatcherReference = luaL_ref(State, LUA_REGISTRYINDEX);
		return 0;
	}

	int DispatchContactCallback(lua_State* State)
	{
		if (!lua_isfunction(State, 1)) return 0;
		const auto* Other = static_cast<const Entity*>(lua_touserdata(State, 2));
		if (!Other) return 0;
		auto* UserData = static_cast<LuaEntity*>(lua_newuserdatauv(State, sizeof(LuaEntity), 0));
		new (UserData) LuaEntity{ *Other };
		luaL_getmetatable(State, EntityMetatableName);
		lua_setmetatable(State, -2);
		lua_remove(State, 2);
		lua_call(State, 1, 0);
		return 0;
	}

	int ExecutePreparedChunk(lua_State* State)
	{
		ScriptInstance* Instance = GetScriptInstance(State);
		if (!Instance || Instance->ExecutingChunkReference == LUA_NOREF || !Instance->ExecutingImporterPath)
			return ReturnHostError(State, "prepared script chunk is unavailable");
		lua_rawgeti(State, LUA_REGISTRYINDEX, Instance->ExecutingChunkReference);
		PushChunkEnvironment(State,
			std::string_view(Instance->ExecutingImporterPath, Instance->ExecutingImporterPathLength));
		(void)lua_setupvalue(State, -2, 1);
		const int Status = lua_pcall(State, 0, 0, 0);
		if (Status != LUA_OK) return lua_error(State);
		return 0;
	}

	void PushChunkEnvironment(lua_State* State, std::string_view ImporterPath)
	{
		ScriptInstance* Instance = GetScriptInstance(State);
		if (!Instance) return;

		lua_createtable(State, 0, 1); // Chunk environment proxy.
		const int EnvironmentIndex = lua_gettop(State);
		lua_createtable(State, 0, 2); // Shared global lookup/write-through metatable.
		lua_rawgeti(State, LUA_REGISTRYINDEX, Instance->EnvironmentReference);
		lua_setfield(State, -2, "__index");
		lua_rawgeti(State, LUA_REGISTRYINDEX, Instance->EnvironmentReference);
		lua_setfield(State, -2, "__newindex");
		lua_setmetatable(State, EnvironmentIndex);

		lua_pushlstring(State, ImporterPath.data(), ImporterPath.size());
		lua_pushcclosure(State, LuaRequire, 1);
		lua_setfield(State, EnvironmentIndex, "require");
	}

} // namespace PulseForge::ScriptEnvironment
