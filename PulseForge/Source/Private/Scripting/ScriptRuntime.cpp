#include "Scripting/ScriptRuntime.h"
#include "Runtime/CookedContentProvider.h"
#include "Scripting/ScriptRuntimeInternal.h"
#include "Scripting/ScriptEnvironment.h"
#include "Assets/AssetImportProvenance.h"
#include "Assets/ScriptSourceMetadata.h"

#if !defined(PF_COOKED_RUNTIME_ONLY)
#include "Assets/AssetPathResolver.h"
#include "Assets/AssetTypes.h"
#endif
#if !defined(PF_COOKED_RUNTIME_ONLY)
#include "Assets/Project.h"
#endif
#include "Core/Log.h"
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
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <optional>
#include <set>
#include <system_error>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace PulseForge
{
	using ScriptRuntimeDetail::CallbackNames;
	using ScriptRuntimeDetail::GetScriptInstance;
	using ScriptRuntimeDetail::InstructionHookQuantum;
	using ScriptRuntimeDetail::LuaMemory;
	using ScriptRuntimeDetail::LuaMemoryPool;
	using ScriptRuntimeDetail::PendingScriptReload;
	using ScriptRuntimeDetail::ScriptModuleImport;
	using ScriptRuntimeDetail::ScriptModuleRuntime;
	using ScriptRuntimeDetail::ScriptModuleSource;
	using ScriptRuntimeDetail::ScriptInstance;
	using ScriptRuntimeDetail::ScriptSourceSnapshot;

	namespace
	{
		ScriptRuntimeError MakeRuntimeError(ScriptRuntimeErrorCode Code, std::string Message)
		{
			return { Code, std::move(Message) };
		}

		ScriptDiagnostic MakeDiagnostic(UUID EntityIdentifier, AssetID ScriptAsset, ScriptDiagnosticCode Code, std::string Message)
		{
			ScriptDiagnostic Result{ EntityIdentifier, ScriptAsset, Code, std::move(Message) };
			Result.SourceAsset = ScriptAsset;
			return Result;
		}

		ScriptDiagnostic MakeScriptDiagnostic(
			UUID EntityIdentifier,
			AssetID ScriptAsset,
			ScriptDiagnosticCode Code,
			std::string Message,
			const ScriptSourceSnapshot& Snapshot,
			std::string_view FallbackPath = {})
		{
			ScriptDiagnostic Result = MakeDiagnostic(EntityIdentifier, ScriptAsset, Code, std::move(Message));
			Result.SourceProject = Snapshot.SourceProject;
			Result.SourceAsset = Snapshot.ScriptAsset;
			const auto AssetForPath = [&](std::string_view Path) -> AssetID
			{
				if (Path == Snapshot.MainSourcePath) return Snapshot.ScriptAsset;
				for (const ScriptModuleSource& Module : Snapshot.Modules)
					if (Path == Module.SourceRelativePath) return Module.SourceAsset;
				return {};
			};
			const auto TryPath = [&](std::string_view Path) -> bool
			{
				if (Path.empty()) return false;
				for (const bool AtPrefix : { false, true })
				{
					const std::string Prefix = (AtPrefix ? "@" : "") + std::string(Path) + ":";
					if (!std::string_view(Result.Message).starts_with(Prefix)) continue;
					const size_t DigitsStart = Prefix.size();
					size_t DigitsEnd = DigitsStart;
					while (DigitsEnd < Result.Message.size() && Result.Message[DigitsEnd] >= '0' && Result.Message[DigitsEnd] <= '9')
						++DigitsEnd;
					if (DigitsEnd == DigitsStart || DigitsEnd >= Result.Message.size() || Result.Message[DigitsEnd] != ':') continue;
					size_t Line = 0;
					const char* Begin = Result.Message.data() + DigitsStart;
					const char* End = Result.Message.data() + DigitsEnd;
					const auto [ParsedEnd, Error] = std::from_chars(Begin, End, Line);
					if (Error != std::errc{} || ParsedEnd != End) continue;
					Result.SourceRelativePath = Path;
					Result.Line = Line;
					if (const AssetID SourceAsset = AssetForPath(Path); !SourceAsset.IsNil()) Result.SourceAsset = SourceAsset;
					return true;
				}
				return false;
			};

			if (!TryPath(Snapshot.MainSourcePath))
			{
				for (const ScriptModuleSource& Module : Snapshot.Modules)
				{
					if (TryPath(Module.SourceRelativePath)) break;
				}
			}
			if (Result.SourceRelativePath.empty())
				Result.SourceRelativePath = FallbackPath.empty() ? Snapshot.MainSourcePath : std::string(FallbackPath);
			if (const AssetID SourceAsset = AssetForPath(Result.SourceRelativePath); !SourceAsset.IsNil()) Result.SourceAsset = SourceAsset;
			Result.SourceFingerprint = Snapshot.Fingerprint;
			return Result;
		}

		void Observe(const ScriptRuntimeObserver& Observer, const ScriptRuntimeObservation& Observation) noexcept
		{
			if (Observer.Observe) Observer.Observe(Observer.Context, Observation);
		}

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
					if (Memory.Pool) Memory.Pool->UsedBytes -= std::min(ReleasedBytes, Memory.Pool->UsedBytes);
				}
				return nullptr;
			}

#if defined(PF_DEBUG)
			if (Memory.FaultHooks && Memory.FaultHooks->RejectAllocation &&
				Memory.ActiveReferencePoint != ScriptRuntimeAllocationPointForTesting::None &&
				Memory.FaultHooks->RejectAllocation(Memory.FaultHooks->Context, Memory.ActiveReferencePoint,
					Memory.ActiveModuleIndex, NewSize))
				return nullptr;
#endif

			const size_t PreviousSize = Pointer ? std::min(OldSize, Memory.UsedBytes) : 0;
			const size_t BaseSize = Memory.UsedBytes - PreviousSize;
			if (BaseSize > Memory.MaximumBytes || NewSize > Memory.MaximumBytes - BaseSize) return nullptr;
			const size_t PoolBaseSize = Memory.Pool ? Memory.Pool->UsedBytes - std::min(PreviousSize, Memory.Pool->UsedBytes) : 0;
			if (Memory.Pool && (PoolBaseSize > Memory.Pool->MaximumBytes || NewSize > Memory.Pool->MaximumBytes - PoolBaseSize))
				return nullptr;

			void* Replacement = std::realloc(Pointer, NewSize);
			if (Replacement)
			{
				Memory.UsedBytes = BaseSize + NewSize;
				if (Memory.Pool) Memory.Pool->UsedBytes = PoolBaseSize + NewSize;
			}
			return Replacement;
		}

		void InstructionBudgetHook(lua_State* State, lua_Debug*)
		{
			ScriptInstance* Instance = GetScriptInstance(State);
			if (!Instance) return;
			if (Instance->InstructionBudget <= InstructionHookQuantum)
			{
				Instance->InstructionBudget = 0;
				lua_pushliteral(State, "script instruction budget exceeded");
				(void)lua_error(State);
				return;
			}
			Instance->InstructionBudget -= InstructionHookQuantum;
		}

		std::string LuaErrorMessage(lua_State* State)
		{
			if (lua_type(State, -1) != LUA_TSTRING)
			{
				lua_pop(State, 1);
				return "Lua returned a non-string error object";
			}
			size_t MessageBytes = 0;
			const char* Message = lua_tolstring(State, -1, &MessageBytes);
			std::string Result(Message, MessageBytes);
			lua_pop(State, 1);
			return Result;
		}

		int StorePreparedChunkReference(lua_State* State)
		{
			ScriptInstance* Instance = GetScriptInstance(State);
			const auto Point = static_cast<ScriptRuntimeAllocationPointForTesting>(lua_tointeger(State, 2));
			const size_t ModuleIndex = static_cast<size_t>(lua_tointeger(State, 3));
#if defined(PF_DEBUG)
			Instance->Memory.ActiveReferencePoint = Point;
			Instance->Memory.ActiveModuleIndex = ModuleIndex;
#else
			(void)Point;
			(void)ModuleIndex;
#endif
			lua_pushvalue(State, 1);
			const int Reference = luaL_ref(State, LUA_REGISTRYINDEX);
#if defined(PF_DEBUG)
			Instance->Memory.ActiveReferencePoint = ScriptRuntimeAllocationPointForTesting::None;
			Instance->Memory.ActiveModuleIndex = 0;
#endif
			lua_pushinteger(State, Reference);
			return 1;
		}

#if defined(PF_DEBUG)
		int PrepareRegistryGrowthForTesting(lua_State* State)
		{
			const lua_Integer Count = lua_tointeger(State, 1);
			for (lua_Integer Index = 0; Index < Count; ++Index)
			{
				lua_pushboolean(State, 1);
				lua_rawseti(State, LUA_REGISTRYINDEX, static_cast<lua_Integer>(3 + Index));
			}
			return 0;
		}

		int ClearRegistryGrowthForTesting(lua_State* State)
		{
			const lua_Integer Count = lua_tointeger(State, 1);
			lua_pushinteger(State, 0);
			lua_rawseti(State, LUA_REGISTRYINDEX, 1);
			for (lua_Integer Index = 0; Index < Count; ++Index)
			{
				lua_pushnil(State);
				lua_rawseti(State, LUA_REGISTRYINDEX, static_cast<lua_Integer>(3 + Index));
			}
			return 0;
		}

		bool RunRegistryGrowthSetup(lua_State* State, lua_CFunction Function, size_t Count)
		{
			const int StackBase = lua_gettop(State);
			if (!lua_checkstack(State, 2)) return false;
			lua_pushcfunction(State, Function);
			lua_pushinteger(State, static_cast<lua_Integer>(Count));
			const bool Succeeded = lua_pcall(State, 1, 0, 0) == LUA_OK;
			lua_settop(State, StackBase);
			return Succeeded;
		}
#endif

		std::expected<int, std::string> ProtectedStorePreparedChunkReference(
			ScriptInstance& Instance, ScriptRuntimeAllocationPointForTesting Point, size_t ModuleIndex)
		{
			lua_State* State = Instance.State;
			const int StackBase = lua_gettop(State) - 1; // The compiled function is the only value above the preparation baseline.
			if (StackBase < 0 || !lua_checkstack(State, 4))
			{
				lua_settop(State, std::max(0, StackBase));
				return std::unexpected("Lua stack limit reached before protecting compiled chunk storage");
			}

			lua_pushcfunction(State, StorePreparedChunkReference);
			lua_insert(State, -2);
			lua_pushinteger(State, static_cast<lua_Integer>(Point));
			lua_pushinteger(State, static_cast<lua_Integer>(ModuleIndex));
			const int Status = lua_pcall(State, 3, 1, 0);
#if defined(PF_DEBUG)
			Instance.Memory.ActiveReferencePoint = ScriptRuntimeAllocationPointForTesting::None;
			Instance.Memory.ActiveModuleIndex = 0;
#endif
			if (Status != LUA_OK)
			{
				std::string Message = LuaErrorMessage(State);
				lua_settop(State, StackBase);
				return std::unexpected("Lua could not allocate while storing the compiled chunk reference: " + Message);
			}

			int IsInteger = 0;
			const lua_Integer Reference = lua_tointegerx(State, -1, &IsInteger);
			lua_settop(State, StackBase);
			if (!IsInteger || Reference < std::numeric_limits<int>::min() || Reference > std::numeric_limits<int>::max())
				return std::unexpected("Lua returned an invalid compiled chunk registry reference");
			return static_cast<int>(Reference);
		}

		std::expected<bool, std::string> RunCallback(
			ScriptInstance& Instance, size_t CallbackIndex, std::optional<double> DeltaSeconds = std::nullopt)
		{
			lua_State* State = Instance.State;
			if (CallbackIndex >= Instance.CallbackNameReferences.size())
				return std::unexpected(std::string("Invalid script lifecycle callback index"));
			if (!lua_checkstack(State, 3)) return std::unexpected(std::string("Lua stack limit reached before lifecycle callback"));

			lua_rawgeti(State, LUA_REGISTRYINDEX, Instance.EnvironmentReference);
			lua_rawgeti(State, LUA_REGISTRYINDEX, Instance.CallbackNameReferences[CallbackIndex]);
			lua_rawget(State, -2);
			lua_remove(State, -2);
			if (lua_isnil(State, -1))
			{
				lua_pop(State, 1);
				return false;
			}
			if (!lua_isfunction(State, -1))
			{
				lua_pop(State, 1);
				std::string Message(CallbackNames[CallbackIndex]);
				Message += " must be a function when defined";
				return std::unexpected(std::move(Message));
			}

			Instance.InstructionBudget = (Instance.MaximumInstructions / InstructionHookQuantum) * InstructionHookQuantum;
			lua_sethook(State, InstructionBudgetHook, LUA_MASKCOUNT, InstructionHookQuantum);
			int ArgumentCount = 0;
			if (DeltaSeconds)
			{
				lua_pushnumber(State, *DeltaSeconds);
				ArgumentCount = 1;
			}
			const auto Callback = static_cast<ScriptRuntimeCallback>(CallbackIndex);
			Observe(Instance.Services.Observer,
				{ .Kind = ScriptRuntimeObservationKind::CallbackEntered,
					.Entity = Instance.BoundEntity.GetUUID(), .ScriptAsset = Instance.Asset, .Callback = Callback });
			const int Status = lua_pcall(State, ArgumentCount, 0, 0);
			lua_sethook(State, nullptr, 0, 0);
			if (Status != LUA_OK) return std::unexpected(LuaErrorMessage(State));
			Observe(Instance.Services.Observer,
				{ .Kind = ScriptRuntimeObservationKind::CallbackCompleted,
					.Entity = Instance.BoundEntity.GetUUID(), .ScriptAsset = Instance.Asset, .Callback = Callback });
			return true;
		}

		std::expected<bool, std::string> RunContactCallback(
			ScriptInstance& Instance, size_t CallbackIndex, const Entity& OtherEntity)
		{
			lua_State* State = Instance.State;
			const int StackBase = lua_gettop(State);
			if (CallbackIndex >= Instance.CallbackNameReferences.size())
				return std::unexpected(std::string("Invalid physics callback index"));
			if (!lua_checkstack(State, 5))
				return std::unexpected(std::string("Lua stack limit reached before physics callback"));

			lua_rawgeti(State, LUA_REGISTRYINDEX, Instance.EnvironmentReference);
			lua_rawgeti(State, LUA_REGISTRYINDEX, Instance.CallbackNameReferences[CallbackIndex]);
			lua_rawget(State, -2);
			lua_remove(State, -2);
			if (lua_isnil(State, -1))
			{
				lua_settop(State, StackBase);
				return false;
			}
			if (!lua_isfunction(State, -1))
			{
				lua_settop(State, StackBase);
				return std::unexpected(std::string(CallbackNames[CallbackIndex]) + " must be a function when defined");
			}

			lua_rawgeti(State, LUA_REGISTRYINDEX, Instance.ContactCallbackDispatcherReference);
			lua_insert(State, -2);
			lua_pushlightuserdata(State, const_cast<Entity*>(&OtherEntity));
			Instance.InstructionBudget = (Instance.MaximumInstructions / InstructionHookQuantum) * InstructionHookQuantum;
			lua_sethook(State, InstructionBudgetHook, LUA_MASKCOUNT, InstructionHookQuantum);
			const auto Callback = static_cast<ScriptRuntimeCallback>(CallbackIndex);
			Observe(Instance.Services.Observer,
				{ .Kind = ScriptRuntimeObservationKind::CallbackEntered,
					.Entity = Instance.BoundEntity.GetUUID(), .ScriptAsset = Instance.Asset, .Callback = Callback });
			const int Status = lua_pcall(State, 2, 0, 0);
			lua_sethook(State, nullptr, 0, 0);
			if (Status != LUA_OK)
			{
				std::string Message = LuaErrorMessage(State);
				lua_settop(State, StackBase);
				return std::unexpected(std::move(Message));
			}
			lua_settop(State, StackBase);
			Observe(Instance.Services.Observer,
				{ .Kind = ScriptRuntimeObservationKind::CallbackCompleted,
					.Entity = Instance.BoundEntity.GetUUID(), .ScriptAsset = Instance.Asset, .Callback = Callback });
			return true;
		}

		int InvokeUIActionTrampoline(lua_State* State)
		{
			const char* Action = static_cast<const char*>(lua_touserdata(State, 1));
			const size_t ActionBytes = static_cast<size_t>(lua_tointeger(State, 2));
			const int EnvironmentReference = static_cast<int>(lua_tointeger(State, 3));
			const int CallbackNameReference = static_cast<int>(lua_tointeger(State, 4));
			lua_rawgeti(State, LUA_REGISTRYINDEX, EnvironmentReference);
			lua_rawgeti(State, LUA_REGISTRYINDEX, CallbackNameReference);
			lua_rawget(State, -2);
			lua_remove(State, -2);
			if (lua_isnil(State, -1)) return 0;
			if (!lua_isfunction(State, -1)) return luaL_error(State, "OnUIAction must be a function when defined");
			lua_pushlstring(State, Action, ActionBytes);
			lua_call(State, 1, 0);
			return 0;
		}

		std::expected<bool, std::string> RunUIActionCallback(ScriptInstance& Instance, std::string_view Action)
		{
			lua_State* State = Instance.State;
			constexpr size_t CallbackIndex = static_cast<size_t>(ScriptRuntimeCallback::OnUIAction);
			const int StackBase = lua_gettop(State);
			if (CallbackIndex >= Instance.CallbackNameReferences.size())
				return std::unexpected(std::string("Invalid runtime UI callback index"));
			if (!lua_checkstack(State, 5)) return std::unexpected(std::string("Lua stack limit reached before runtime UI callback"));
			lua_pushcfunction(State, InvokeUIActionTrampoline);
			lua_pushlightuserdata(State, const_cast<char*>(Action.data()));
			lua_pushinteger(State, static_cast<lua_Integer>(Action.size()));
			lua_pushinteger(State, static_cast<lua_Integer>(Instance.EnvironmentReference));
			lua_pushinteger(State, static_cast<lua_Integer>(Instance.CallbackNameReferences[CallbackIndex]));
			Instance.InstructionBudget = (Instance.MaximumInstructions / InstructionHookQuantum) * InstructionHookQuantum;
			lua_sethook(State, InstructionBudgetHook, LUA_MASKCOUNT, InstructionHookQuantum);
			Observe(Instance.Services.Observer,
				{ .Kind = ScriptRuntimeObservationKind::CallbackEntered,
					.Entity = Instance.BoundEntity.GetUUID(), .ScriptAsset = Instance.Asset,
					.Callback = ScriptRuntimeCallback::OnUIAction });
			const int Status = lua_pcall(State, 4, 0, 0);
			lua_sethook(State, nullptr, 0, 0);
			if (Status != LUA_OK)
			{
				std::string Message = LuaErrorMessage(State);
				lua_settop(State, StackBase);
				return std::unexpected(std::move(Message));
			}
			lua_settop(State, StackBase);
			Observe(Instance.Services.Observer,
				{ .Kind = ScriptRuntimeObservationKind::CallbackCompleted,
					.Entity = Instance.BoundEntity.GetUUID(), .ScriptAsset = Instance.Asset,
					.Callback = ScriptRuntimeCallback::OnUIAction });
			return true;
		}

		std::string GenericPathString(const std::filesystem::path& Path)
		{
			const std::u8string Encoded = Path.generic_u8string();
			return std::string(reinterpret_cast<const char*>(Encoded.data()), Encoded.size());
		}

		struct ScriptSourceReadError final
		{
			std::string Message;
		};

		std::expected<std::string, ScriptSourceReadError> ReadTextFile(
			const std::filesystem::path& Path, size_t MaximumBytes)
		{
			try
			{
				std::error_code FileError;
				const uintmax_t FileSize = std::filesystem::file_size(Path, FileError);
				if (FileError)
					return std::unexpected(ScriptSourceReadError{ "Could not determine script source size: " + FileError.message() });
				if (FileSize > MaximumBytes || FileSize > static_cast<uintmax_t>(std::numeric_limits<size_t>::max()) ||
					FileSize > static_cast<uintmax_t>(std::numeric_limits<std::streamsize>::max()))
					return std::unexpected(ScriptSourceReadError{ "Script source exceeds the configured source limit" });

				std::ifstream Input(Path, std::ios::binary);
				if (!Input.is_open()) return std::unexpected(ScriptSourceReadError{ "Could not open the managed script source file" });
				std::string Source(static_cast<size_t>(FileSize), '\0');
				if (!Source.empty())
				{
					Input.read(Source.data(), static_cast<std::streamsize>(Source.size()));
					if (!Input || Input.bad())
						return std::unexpected(ScriptSourceReadError{ "Could not read the complete managed script source file" });
				}
				return Source;
			}
			catch (const std::exception& Exception)
			{
				return std::unexpected(ScriptSourceReadError{ std::string("Could not read script source: ") + Exception.what() });
			}
		}
	} // namespace

	struct ScriptRuntime::Impl final
	{
		const Project* SourceProject = nullptr;
		std::shared_ptr<const CookedContentProvider> CookedAssets;
		ScriptRuntimeDesc Description;
		ScriptRuntimeServices Services;
		Scene* Source = nullptr;
		LuaMemoryPool MemoryPool;
		std::map<UUID, std::unique_ptr<ScriptInstance>> Scripts;
		struct FailedAttachment
		{
			Entity BoundEntity;
			AssetID Asset;
		};
		std::map<UUID, FailedAttachment> FailedAttachments;
		std::vector<ScriptDiagnostic> Diagnostics;
		std::mutex ReloadMutex;
		std::deque<PendingScriptReload> PendingReloads;
		std::map<UUID, uint64_t> LatestReloadTickets;
		std::set<uint64_t> CancelledReloadTickets;
		std::set<uint64_t> ActiveReloadTickets;
		uint64_t NextReloadTicket = 1;
		bool Stopping = false;

		Impl(const Project* ProjectValue, std::shared_ptr<const CookedContentProvider> Provider,
			ScriptRuntimeDesc RuntimeDescription, ScriptRuntimeServices RuntimeServices)
			: SourceProject(ProjectValue), CookedAssets(std::move(Provider)), Description(RuntimeDescription), Services(RuntimeServices)
		{
			MemoryPool.MaximumBytes = Description.MaxTotalLuaMemoryBytes;
		}

		void RecordDiagnostic(ScriptDiagnostic Diagnostic)
		{
			constexpr size_t MaximumRetainedDiagnostics = 128;
			const auto SameFailure = [&Diagnostic](const ScriptDiagnostic& Existing)
			{
				return Existing.Entity == Diagnostic.Entity && Existing.ScriptAsset == Diagnostic.ScriptAsset &&
					Existing.Code == Diagnostic.Code && Existing.Message == Diagnostic.Message &&
					Existing.SourceRelativePath == Diagnostic.SourceRelativePath && Existing.Line == Diagnostic.Line &&
					Existing.SourceFingerprint == Diagnostic.SourceFingerprint && Existing.SourceProject == Diagnostic.SourceProject &&
					Existing.SourceAsset == Diagnostic.SourceAsset;
			};
			if (std::any_of(Diagnostics.begin(), Diagnostics.end(), SameFailure)) return;
			if (Diagnostics.size() == MaximumRetainedDiagnostics) Diagnostics.erase(Diagnostics.begin());
			PF_ERROR("Script on entity {0} (asset {1}) failed: {2}", Diagnostic.Entity.ToString(), Diagnostic.ScriptAsset.ToString(),
				Diagnostic.Message);
			Observe(Services.Observer,
				{ .Kind = ScriptRuntimeObservationKind::Diagnostic,
					.Entity = Diagnostic.Entity, .ScriptAsset = Diagnostic.ScriptAsset, .Diagnostic = Diagnostic.Code });
			Diagnostics.push_back(std::move(Diagnostic));
		}

		void RecordPendingModuleFailure(ScriptInstance& Instance)
		{
			if (!Instance.ModuleFailurePending) return;
			Instance.ModuleFailurePending = false;
			if (Instance.SourceSnapshot)
				RecordDiagnostic(MakeScriptDiagnostic(Instance.BoundEntity.GetUUID(), Instance.Asset,
					ScriptDiagnosticCode::ModuleLoadFailed, Instance.ModuleFailureMessage.data(), *Instance.SourceSnapshot,
					Instance.SourceSnapshot->MainSourcePath));
			else
				RecordDiagnostic(MakeDiagnostic(Instance.BoundEntity.GetUUID(), Instance.Asset,
					ScriptDiagnosticCode::ModuleLoadFailed, Instance.ModuleFailureMessage.data()));
			Instance.Faulted = true;
		}

		std::expected<std::shared_ptr<const ScriptSourceSnapshot>, ScriptDiagnostic> ResolveSnapshot(
			const AssetID& ScriptAsset, UUID EntityIdentifier)
		{
			struct SourceFile final
			{
				AssetID Asset;
				std::string RelativePath;
				std::string Source;
			};

			const auto ReadProjectPath = [&](const std::string& RelativePath) -> std::expected<SourceFile, ScriptSourceReadError>
			{
				if (CookedAssets)
				{
					const CookedAssetEntry* Entry = CookedAssets->FindScriptByProjectPath(RelativePath);
					if (!Entry)
						return std::unexpected(ScriptSourceReadError{ "Declared script module is not present in the cooked catalog: " + RelativePath });
					auto Bytes = CookedAssets->ReadScriptByProjectPath(RelativePath);
					if (!Bytes) return std::unexpected(ScriptSourceReadError{ Bytes.error().Message });
					if (Bytes->size() > Description.MaxSourceBytes)
						return std::unexpected(ScriptSourceReadError{
							"Script source exceeds the configured per-file source limit: " + RelativePath });
					return SourceFile{ Entry->Asset, RelativePath,
						std::string(reinterpret_cast<const char*>(Bytes->data()), Bytes->size()) };
				}
#if !defined(PF_COOKED_RUNTIME_ONLY)
				if (!SourceProject)
					return std::unexpected(ScriptSourceReadError{ "No source project or cooked asset provider is available" });
				const std::filesystem::path RequestedPath(std::u8string(
					reinterpret_cast<const char8_t*>(RelativePath.data()), RelativePath.size()));
				std::optional<AssetRecord> Exact;
				std::string FoldedRequested = RelativePath;
				std::transform(FoldedRequested.begin(), FoldedRequested.end(), FoldedRequested.begin(),
					[](unsigned char Character) { return static_cast<char>(std::tolower(Character)); });
				for (const AssetRecord& Candidate : SourceProject->GetAssetRegistry().GetAssets())
				{
					if (!HasAssetTypeExtension(Candidate.ProjectRelativePath, AssetFileType::Script)) continue;
					const std::string CandidatePath = GenericPathString(Candidate.ProjectRelativePath.lexically_normal());
					if (Candidate.ProjectRelativePath.lexically_normal() == RequestedPath.lexically_normal())
					{
						if (Exact)
							return std::unexpected(ScriptSourceReadError{ "Ambiguous script module path in the source registry: " + RelativePath });
					Exact = Candidate;
					continue;
					}
					std::string FoldedCandidate = CandidatePath;
					std::transform(FoldedCandidate.begin(), FoldedCandidate.end(), FoldedCandidate.begin(),
						[](unsigned char Character) { return static_cast<char>(std::tolower(Character)); });
					if (FoldedCandidate == FoldedRequested)
						return std::unexpected(ScriptSourceReadError{
							"Case-equivalent script module path is ambiguous in the source registry: " + RelativePath });
				}
				if (!Exact)
					return std::unexpected(ScriptSourceReadError{ "Script module path is not present in the source registry: " + RelativePath });
				const auto Resolved = AssetPathResolver::ResolveManagedSourcePath(SourceProject->GetRootPath(), Exact->ProjectRelativePath);
				if (!Resolved) return std::unexpected(ScriptSourceReadError{ Resolved.error().Message });
				auto Source = ReadTextFile(*Resolved, Description.MaxSourceBytes);
				if (!Source) return std::unexpected(Source.error());
				return SourceFile{ Exact->ID, RelativePath, std::move(*Source) };
#else
				return std::unexpected(ScriptSourceReadError{ "Cooked script provider is unavailable" });
#endif
			};

			SourceFile MainFile;
			if (CookedAssets)
			{
				const CookedAssetEntry* Entry = CookedAssets->FindScript(ScriptAsset);
				if (!Entry) return std::unexpected(MakeDiagnostic(EntityIdentifier, ScriptAsset,
					ScriptDiagnosticCode::AssetNotFound, "Script asset is not present in the cooked catalog"));
				MainFile.Asset = ScriptAsset;
				MainFile.RelativePath = Entry->SourceRelativePath;
				auto Bytes = CookedAssets->ReadScript(ScriptAsset);
				if (!Bytes) return std::unexpected(MakeDiagnostic(EntityIdentifier, ScriptAsset,
					ScriptDiagnosticCode::FileReadFailed, Bytes.error().Message));
				if (Bytes->size() > Description.MaxSourceBytes)
					return std::unexpected(MakeDiagnostic(EntityIdentifier, ScriptAsset,
						ScriptDiagnosticCode::FileReadFailed, "Cooked script source exceeds the configured source limit"));
				MainFile.Source.assign(reinterpret_cast<const char*>(Bytes->data()), Bytes->size());
			}
#if !defined(PF_COOKED_RUNTIME_ONLY)
			else
			{
				if (!SourceProject) return std::unexpected(MakeDiagnostic(EntityIdentifier, ScriptAsset,
					ScriptDiagnosticCode::AssetNotFound, "No source project or cooked asset provider is available"));
				const auto Record = SourceProject->GetAssetRegistry().Find(ScriptAsset);
				if (!Record) return std::unexpected(MakeDiagnostic(EntityIdentifier, ScriptAsset,
					ScriptDiagnosticCode::AssetNotFound, "Script UUID is not present in the project asset registry"));
				if (!HasAssetTypeExtension(Record->ProjectRelativePath, AssetFileType::Script))
					return std::unexpected(MakeDiagnostic(EntityIdentifier, ScriptAsset,
						ScriptDiagnosticCode::WrongAssetType, "Script asset must reference a .lua source file"));
				MainFile.Asset = ScriptAsset;
				MainFile.RelativePath = GenericPathString(Record->ProjectRelativePath.lexically_normal());
				const auto ResolvedPath = AssetPathResolver::ResolveManagedSourcePath(SourceProject->GetRootPath(), Record->ProjectRelativePath);
				if (!ResolvedPath) return std::unexpected(MakeDiagnostic(EntityIdentifier, ScriptAsset,
					ScriptDiagnosticCode::FileReadFailed, ResolvedPath.error().Message));
				auto Text = ReadTextFile(*ResolvedPath, Description.MaxSourceBytes);
				if (!Text) return std::unexpected(MakeDiagnostic(EntityIdentifier, ScriptAsset,
					ScriptDiagnosticCode::FileReadFailed, Text.error().Message));
				MainFile.Source = std::move(*Text);
			}
#else
			else return std::unexpected(MakeDiagnostic(EntityIdentifier, ScriptAsset,
				ScriptDiagnosticCode::AssetNotFound, "Cooked script provider is unavailable"));
#endif

			auto Result = std::make_shared<ScriptSourceSnapshot>();
			size_t TotalSourceBytes = MainFile.Source.size();
			if (TotalSourceBytes > Description.MaxTotalScriptSourceBytes)
				return std::unexpected(MakeDiagnostic(EntityIdentifier, ScriptAsset,
					ScriptDiagnosticCode::FileReadFailed, "Script snapshot exceeds the configured total source limit"));
			Result->ScriptAsset = ScriptAsset;
#if !defined(PF_COOKED_RUNTIME_ONLY)
			Result->SourceProject = SourceProject ? SourceProject->GetDescription().ID : UUID{};
	#else
			Result->SourceProject = {};
	#endif
			Result->MainSourcePath = MainFile.RelativePath;
			Result->MainSource = std::move(MainFile.Source);
			AssetFingerprintBuilder Fingerprint("PulseForge.ScriptSourceSnapshot.v1");
			Fingerprint.UpdateText("asset", ScriptAsset.ToString());
			Fingerprint.UpdateText("main-path", Result->MainSourcePath);
			Fingerprint.UpdateText("main-source", Result->MainSource);
			auto MainMetadata = ParseScriptSourceMetadata(Result->MainSource);
			if (!MainMetadata)
			{
				auto Diagnostic = MakeDiagnostic(EntityIdentifier, ScriptAsset, ScriptDiagnosticCode::InvalidComponent,
					"Invalid script metadata: " + MainMetadata.error().Message);
				Diagnostic.SourceRelativePath = Result->MainSourcePath;
				Diagnostic.SourceProject = Result->SourceProject;
				Diagnostic.SourceAsset = ScriptAsset;
				Diagnostic.Line = MainMetadata.error().Line;
				auto Identity = Fingerprint.Finish();
				Diagnostic.SourceFingerprint = Identity.Algorithm + ":" + Identity.DigestHex;
				return std::unexpected(std::move(Diagnostic));
			}
			Result->Metadata = std::move(*MainMetadata);
			if (Result->Metadata.FloatProperties.size() > MaximumScriptFloatPropertyCount)
				return std::unexpected(MakeDiagnostic(EntityIdentifier, ScriptAsset, ScriptDiagnosticCode::InvalidComponent,
					"Script metadata exceeds the authored float property limit"));

			std::map<std::string, ScriptModuleSource, std::less<>> ModuleSources;
			std::unordered_set<std::string> Visiting;
			std::function<std::expected<void, std::string>(std::string_view, const ScriptSourceMetadata&,
				std::vector<ScriptModuleImport>&)> ResolveImports;
			std::function<std::expected<void, std::string>(std::string)> VisitModule;
			ResolveImports = [&](std::string_view ImporterPath, const ScriptSourceMetadata& Metadata,
				std::vector<ScriptModuleImport>& Imports) -> std::expected<void, std::string>
			{
				std::unordered_set<std::string> Targets;
				for (const std::string& DeclaredName : Metadata.Modules)
				{
					auto ModulePath = ResolveScriptModuleAssetPath(ImporterPath, DeclaredName);
					if (!ModulePath) return std::unexpected("Invalid declared module '" + DeclaredName + "': " + ModulePath.error().Message);
					const std::string FilePath = GenericPathString(*ModulePath);
					std::filesystem::path IdentityPath = *ModulePath;
					IdentityPath.replace_extension();
					const std::string ModuleID = GenericPathString(IdentityPath);
					if (!Targets.insert(ModuleID).second)
						return std::unexpected("Ambiguous script module aliases resolve to the same module: " + DeclaredName);
					Imports.push_back({ DeclaredName, ModuleID });
					auto Resolved = VisitModule(FilePath);
					if (!Resolved) return Resolved;
				}
				return {};
			};
			VisitModule = [&](std::string FilePath) -> std::expected<void, std::string>
			{
				if (FilePath == Result->MainSourcePath)
					return std::unexpected("Script module graph refers back to its main script: " + FilePath);
				std::filesystem::path IdentityPath(std::u8string(
					reinterpret_cast<const char8_t*>(FilePath.data()), FilePath.size()));
				IdentityPath.replace_extension();
				const std::string ModuleID = GenericPathString(IdentityPath.lexically_normal());
				if (Visiting.contains(ModuleID)) return std::unexpected("Script module dependency cycle includes " + ModuleID);
				if (ModuleSources.contains(ModuleID)) return {};
				if (ModuleSources.size() >= Description.MaxScriptModuleCount)
					return std::unexpected("Script module graph exceeds the configured module count limit");
				if (Result->MainSourcePath.empty())
					return std::unexpected("Cooked script manifest lacks logical source paths required by module declarations");

				auto Source = ReadProjectPath(FilePath);
				if (!Source)
				{
					Fingerprint.UpdateText("unavailable-module", FilePath);
					return std::unexpected(Source.error().Message);
				}
				Fingerprint.UpdateText("module-path", ModuleID);
				Fingerprint.UpdateText("module-source", Source->Source);
				if (Source->Source.size() > Description.MaxTotalScriptSourceBytes - TotalSourceBytes)
					return std::unexpected("Script module graph exceeds the configured total source limit");
				auto Metadata = ParseScriptSourceMetadata(Source->Source);
				if (!Metadata) return std::unexpected("Invalid metadata in module " + ModuleID + " at line " +
					std::to_string(Metadata.error().Line) + ": " + Metadata.error().Message);
				if (!Metadata->FloatProperties.empty())
					return std::unexpected("Only the main script may declare authored float properties: " + ModuleID);

				ScriptModuleSource Module;
				Module.SourceAsset = Source->Asset;
				Module.ProjectRelativePath = ModuleID;
				Module.SourceRelativePath = std::move(Source->RelativePath);
				Module.Source = std::move(Source->Source);
				TotalSourceBytes += Module.Source.size();
				auto [It, Inserted] = ModuleSources.emplace(ModuleID, std::move(Module));
				(void)Inserted;
				Visiting.insert(ModuleID);
				auto ImportsResult = ResolveImports(FilePath, *Metadata, It->second.Imports);
				if (!ImportsResult) return ImportsResult;
				Visiting.erase(ModuleID);
				return {};
			};

			auto ImportsResult = ResolveImports(Result->MainSourcePath, Result->Metadata, Result->MainImports);
			if (!ImportsResult)
			{
				auto Diagnostic = MakeDiagnostic(EntityIdentifier, ScriptAsset, ScriptDiagnosticCode::FileReadFailed,
					"Could not resolve script module graph: " + ImportsResult.error());
				Diagnostic.SourceRelativePath = Result->MainSourcePath;
				auto Identity = Fingerprint.Finish();
				Diagnostic.SourceFingerprint = Identity.Algorithm + ":" + Identity.DigestHex;
				return std::unexpected(std::move(Diagnostic));
			}
			Result->Modules.reserve(ModuleSources.size());
			for (auto& [Path, Module] : ModuleSources)
			{
				(void)Path;
				Result->Modules.push_back(std::move(Module));
			}
			const AssetContentFingerprint Identity = Fingerprint.Finish();
			Result->Fingerprint = Identity.Algorithm + ":" + Identity.DigestHex;
			return std::shared_ptr<const ScriptSourceSnapshot>(std::move(Result));
		}

		std::expected<void, ScriptDiagnostic> ValidateComponentSnapshot(
			const ScriptComponent& Component, const ScriptSourceSnapshot& Snapshot, UUID EntityIdentifier)
		{
			if (auto Validation = Component.Validate(); !Validation)
			{
				return std::unexpected(MakeDiagnostic(EntityIdentifier,
					Component.ScriptAsset, ScriptDiagnosticCode::InvalidComponent, Validation.error().Message));
			}
			if (Snapshot.ScriptAsset != Component.ScriptAsset)
				return std::unexpected(MakeDiagnostic(EntityIdentifier, Component.ScriptAsset,
					ScriptDiagnosticCode::ReloadRejected, "Resolved script snapshot asset does not match the ScriptComponent"));
			for (const ScriptFloatPropertyDeclaration& Declaration : Snapshot.Metadata.FloatProperties)
			{
				if (!IsValidScriptPropertyIdentifier(Declaration.ID) || !std::isfinite(Declaration.DefaultValue) ||
					!std::isfinite(Declaration.Minimum) || !std::isfinite(Declaration.Maximum) ||
					Declaration.Minimum > Declaration.Maximum || Declaration.DefaultValue < Declaration.Minimum ||
					Declaration.DefaultValue > Declaration.Maximum)
					return std::unexpected(MakeDiagnostic(EntityIdentifier, Component.ScriptAsset,
						ScriptDiagnosticCode::InvalidComponent, "Script snapshot contains an invalid float property declaration"));
				const auto Override = Component.FloatProperties.find(Declaration.ID);
				if (Override != Component.FloatProperties.end() &&
					(Override->second < Declaration.Minimum || Override->second > Declaration.Maximum))
					return std::unexpected(MakeDiagnostic(EntityIdentifier, Component.ScriptAsset,
						ScriptDiagnosticCode::InvalidComponent, "Authored script property override is outside its declared range: " + Declaration.ID));
			}
			return {};
		}

		std::expected<std::unique_ptr<ScriptInstance>, ScriptDiagnostic> PrepareInstance(
			Scene& SceneValue, const Entity& EntityValue, const ScriptComponent& Component,
			std::shared_ptr<const ScriptSourceSnapshot> Snapshot)
		{
			const UUID EntityIdentifier = EntityValue.GetUUID();
			if (!Snapshot) return std::unexpected(MakeDiagnostic(EntityIdentifier, Component.ScriptAsset,
				ScriptDiagnosticCode::ReloadRejected, "No immutable script source snapshot was supplied"));
			if (auto Validation = ValidateComponentSnapshot(Component, *Snapshot, EntityIdentifier); !Validation)
			{
				ScriptDiagnostic Diagnostic = Validation.error();
				Diagnostic.SourceRelativePath = Snapshot->MainSourcePath;
				return std::unexpected(std::move(Diagnostic));
			}

			auto Instance = std::make_unique<ScriptInstance>();
			Instance->BoundEntity = EntityValue;
			Instance->Asset = Component.ScriptAsset;
			Instance->SourceSnapshot = std::move(Snapshot);
			Instance->SourceScene = &SceneValue;
			Instance->SourceProject = SourceProject;
			Instance->CookedAssets = CookedAssets;
			Instance->Services = Services;
			Instance->Memory.MaximumBytes = Description.MaxLuaMemoryBytes;
			Instance->Memory.Pool = &MemoryPool;
#if defined(PF_DEBUG)
			Instance->Memory.FaultHooks = Description.AllocationFaultHooksForTesting;
#endif
			Instance->MaximumInstructions = Description.MaxInstructionsPerCallback;
			Instance->State = lua_newstate(BoundedLuaAllocator, &Instance->Memory, luaL_makeseed(nullptr));
			if (!Instance->State)
			{
				return std::unexpected(MakeDiagnostic(EntityIdentifier, Component.ScriptAsset,
					ScriptDiagnosticCode::LuaInitializationFailed, "Could not create a Lua state within the configured memory limit"));
			}
			*static_cast<ScriptInstance**>(lua_getextraspace(Instance->State)) = Instance.get();

			lua_pushcfunction(Instance->State, ScriptEnvironment::OpenRestrictedLibraries);
			if (lua_pcall(Instance->State, 0, 0, 0) != LUA_OK)
				return std::unexpected(MakeScriptDiagnostic(EntityIdentifier, Component.ScriptAsset,
					ScriptDiagnosticCode::LuaInitializationFailed, LuaErrorMessage(Instance->State), *Instance->SourceSnapshot));

#if defined(PF_DEBUG)
			const size_t RegistryPadding = Instance->Memory.FaultHooks
				? Instance->Memory.FaultHooks->RegistryPaddingForTesting : 0;
			if (RegistryPadding != 0 && !RunRegistryGrowthSetup(
				Instance->State, PrepareRegistryGrowthForTesting, RegistryPadding))
				return std::unexpected(MakeScriptDiagnostic(EntityIdentifier, Component.ScriptAsset,
					ScriptDiagnosticCode::LuaInitializationFailed,
					"Could not prepare deterministic Lua registry growth for the fault test", *Instance->SourceSnapshot));
#endif

			const std::string MainChunkName = "@" + (Instance->SourceSnapshot->MainSourcePath.empty()
				? std::string("PulseForge script") : Instance->SourceSnapshot->MainSourcePath);
			const int MainLoadStatus = luaL_loadbufferx(Instance->State, Instance->SourceSnapshot->MainSource.data(),
				Instance->SourceSnapshot->MainSource.size(), MainChunkName.c_str(), "t");
			if (MainLoadStatus != LUA_OK)
			{
				auto Diagnostic = MakeScriptDiagnostic(EntityIdentifier, Component.ScriptAsset,
					ScriptDiagnosticCode::CompileFailed, LuaErrorMessage(Instance->State), *Instance->SourceSnapshot,
					Instance->SourceSnapshot->MainSourcePath);
				return std::unexpected(std::move(Diagnostic));
			}
			auto MainReference = ProtectedStorePreparedChunkReference(
				*Instance, ScriptRuntimeAllocationPointForTesting::MainChunkReference, 0);
			if (!MainReference)
				return std::unexpected(MakeScriptDiagnostic(EntityIdentifier, Component.ScriptAsset,
					ScriptDiagnosticCode::LuaInitializationFailed,
					"Could not retain the compiled main script: " + MainReference.error(), *Instance->SourceSnapshot,
					Instance->SourceSnapshot->MainSourcePath));
			Instance->MainChunkReference = *MainReference;

			Instance->Modules.reserve(Instance->SourceSnapshot->Modules.size());
			for (size_t ModuleIndex = 0; ModuleIndex < Instance->SourceSnapshot->Modules.size(); ++ModuleIndex)
			{
				const ScriptModuleSource& Module = Instance->SourceSnapshot->Modules[ModuleIndex];
				const std::string ChunkName = "@" + Module.SourceRelativePath;
				const int Status = luaL_loadbufferx(Instance->State, Module.Source.data(), Module.Source.size(), ChunkName.c_str(), "t");
				if (Status != LUA_OK)
				{
					auto Diagnostic = MakeScriptDiagnostic(EntityIdentifier, Component.ScriptAsset,
						ScriptDiagnosticCode::CompileFailed, LuaErrorMessage(Instance->State), *Instance->SourceSnapshot,
						Module.SourceRelativePath);
					return std::unexpected(std::move(Diagnostic));
				}
				auto ModuleReference = ProtectedStorePreparedChunkReference(
					*Instance, ScriptRuntimeAllocationPointForTesting::ModuleChunkReference, ModuleIndex);
				if (!ModuleReference)
					return std::unexpected(MakeScriptDiagnostic(EntityIdentifier, Component.ScriptAsset,
						ScriptDiagnosticCode::LuaInitializationFailed,
						"Could not retain compiled module '" + Module.SourceRelativePath + "': " + ModuleReference.error(),
						*Instance->SourceSnapshot, Module.SourceRelativePath));

				ScriptModuleRuntime RuntimeModule;
				RuntimeModule.ProjectRelativePath = Module.ProjectRelativePath;
				RuntimeModule.Imports = Module.Imports;
				RuntimeModule.CompiledChunkReference = *ModuleReference;
				Instance->Modules.push_back(std::move(RuntimeModule));
			}

#if defined(PF_DEBUG)
			if (RegistryPadding != 0 && !RunRegistryGrowthSetup(
				Instance->State, ClearRegistryGrowthForTesting, RegistryPadding))
				return std::unexpected(MakeScriptDiagnostic(EntityIdentifier, Component.ScriptAsset,
					ScriptDiagnosticCode::LuaInitializationFailed,
					"Could not clear deterministic Lua registry growth after the fault test", *Instance->SourceSnapshot));
#endif

			// Environment setup creates callbacks and user bindings only after every compiled chunk
			// has been safely retained. Compilation itself does not execute the script.
			lua_pushcfunction(Instance->State, ScriptEnvironment::Initialize);
			if (lua_pcall(Instance->State, 0, 0, 0) != LUA_OK)
				return std::unexpected(MakeScriptDiagnostic(EntityIdentifier, Component.ScriptAsset,
					ScriptDiagnosticCode::LuaInitializationFailed, LuaErrorMessage(Instance->State), *Instance->SourceSnapshot));
			return Instance;
		}

		std::expected<void, std::string> ActivateInstance(ScriptInstance& Instance)
		{
			lua_State* State = Instance.State;
			if (!lua_checkstack(State, 1))
				return std::unexpected(std::string("Lua stack limit reached before executing the prepared script"));
			Instance.ExecutingChunkReference = Instance.MainChunkReference;
			Instance.ExecutingImporterPath = Instance.SourceSnapshot->MainSourcePath.data();
			Instance.ExecutingImporterPathLength = Instance.SourceSnapshot->MainSourcePath.size();
			lua_rawgeti(State, LUA_REGISTRYINDEX, Instance.ExecuteChunkCallbackReference);
			Instance.InstructionBudget = (Instance.MaximumInstructions / InstructionHookQuantum) * InstructionHookQuantum;
			lua_sethook(State, InstructionBudgetHook, LUA_MASKCOUNT, InstructionHookQuantum);
			const int ExecuteStatus = lua_pcall(State, 0, 0, 0);
			lua_sethook(State, nullptr, 0, 0);
			Instance.ExecutingChunkReference = LUA_NOREF;
			Instance.ExecutingImporterPath = nullptr;
			Instance.ExecutingImporterPathLength = 0;
			if (ExecuteStatus != LUA_OK) return std::unexpected(LuaErrorMessage(State));

			if (auto CreateResult = RunCallback(Instance, 0); !CreateResult)
				return std::unexpected(std::move(CreateResult.error()));
			if (Instance.ModuleFailurePending)
				return std::unexpected(std::string(Instance.ModuleFailureMessage.data()));
			return {};
		}

		std::expected<std::unique_ptr<ScriptInstance>, ScriptDiagnostic> CreateInstance(
			Scene& SceneValue, const Entity& EntityValue, const ScriptComponent& Component)
		{
			const UUID EntityIdentifier = EntityValue.GetUUID();
			auto Snapshot = ResolveSnapshot(Component.ScriptAsset, EntityIdentifier);
			if (!Snapshot) return std::unexpected(Snapshot.error());
			auto Instance = PrepareInstance(SceneValue, EntityValue, Component, *Snapshot);
			if (!Instance) return std::unexpected(Instance.error());
			if (auto Activated = ActivateInstance(**Instance); !Activated)
			{
				RecordPendingModuleFailure(**Instance);
				return std::unexpected(MakeScriptDiagnostic(EntityIdentifier, Component.ScriptAsset,
					ScriptDiagnosticCode::InitializationFailed, std::move(Activated.error()), **Snapshot,
					(*Snapshot)->MainSourcePath));
			}
			return Instance;
		}

		std::expected<void, std::string> InvokeDestroy(ScriptInstance& Instance)
		{
			if (Instance.DestroyInvoked) return {};
			Instance.DestroyInvoked = true;
			Instance.Destroying = true;
			struct DestroyingReset final
			{
				ScriptInstance& Instance;
				~DestroyingReset() { Instance.Destroying = false; }
			} Reset{ Instance };

			auto DestroyResult = RunCallback(Instance, 2);
			RecordPendingModuleFailure(Instance);
			if (!DestroyResult)
			{
				const std::string Message = std::move(DestroyResult.error());
				RecordDiagnostic(MakeScriptDiagnostic(Instance.BoundEntity.GetUUID(), Instance.Asset,
					ScriptDiagnosticCode::DestroyFailed, Message, *Instance.SourceSnapshot,
					Instance.SourceSnapshot->MainSourcePath));
				return std::unexpected(Message);
			}
			return {};
		}

		void Attach(Scene& SceneValue, const Entity& EntityValue, const ScriptComponent& Component)
		{
			try
			{
				auto Instance = CreateInstance(SceneValue, EntityValue, Component);
				if (!Instance)
				{
					RecordDiagnostic(std::move(Instance.error()));
					FailedAttachments.insert_or_assign(EntityValue.GetUUID(), FailedAttachment{ EntityValue, Component.ScriptAsset });
					return;
				}
				Scripts.insert_or_assign(EntityValue.GetUUID(), std::move(*Instance));
				Observe(Services.Observer,
					{ .Kind = ScriptRuntimeObservationKind::ScriptAttached, .Entity = EntityValue.GetUUID(), .ScriptAsset = Component.ScriptAsset });
				FailedAttachments.erase(EntityValue.GetUUID());
			}
			catch (const std::exception& Exception)
			{
				RecordDiagnostic(MakeDiagnostic(EntityValue.GetUUID(), Component.ScriptAsset, ScriptDiagnosticCode::LuaInitializationFailed,
					std::string("Could not initialize Lua script instance: ") + Exception.what()));
				FailedAttachments.insert_or_assign(EntityValue.GetUUID(), FailedAttachment{ EntityValue, Component.ScriptAsset });
			}
		}

		void SynchronizeAttachments(Scene& SceneValue)
		{
			for (auto Iterator = Scripts.begin(); Iterator != Scripts.end();)
			{
				const auto CurrentEntity = SceneValue.FindEntity(Iterator->first);
				bool Remove = !CurrentEntity || *CurrentEntity != Iterator->second->BoundEntity;
				if (CurrentEntity && *CurrentEntity == Iterator->second->BoundEntity)
				{
					const auto Component = CurrentEntity->GetScript();
					if (!Component)
					{
						RecordDiagnostic(MakeDiagnostic(
							Iterator->first, Iterator->second->Asset, ScriptDiagnosticCode::InvalidComponent, Component.error().Message));
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

				(void)InvokeDestroy(*Iterator->second);
				Iterator = Scripts.erase(Iterator);
			}

			for (auto Iterator = FailedAttachments.begin(); Iterator != FailedAttachments.end();)
			{
				const auto CurrentEntity = SceneValue.FindEntity(Iterator->first);
				const bool SameIncarnation = CurrentEntity && *CurrentEntity == Iterator->second.BoundEntity;
				const auto Component = SameIncarnation
										   ? CurrentEntity->GetScript()
										   : std::expected<std::optional<ScriptComponent>, SceneError>(std::optional<ScriptComponent>{});
				if (!Component || !Component->has_value() || !Component->value().Enabled ||
					Component->value().ScriptAsset != Iterator->second.Asset)
					Iterator = FailedAttachments.erase(Iterator);
				else ++Iterator;
			}

			for (const Entity& EntityValue : SceneValue.GetEntities())
			{
				if (!EntityValue.IsValid()) continue;
				const auto Component = EntityValue.GetScript();
				if (!Component || !Component->has_value() || !Component->value().Enabled || Scripts.contains(EntityValue.GetUUID()))
					continue;

				const auto Failed = FailedAttachments.find(EntityValue.GetUUID());
				if (Failed != FailedAttachments.end() && Failed->second.BoundEntity == EntityValue &&
					Failed->second.Asset == Component->value().ScriptAsset)
					continue;
				Attach(SceneValue, EntityValue, Component->value());
			}
		}
	};

#if !defined(PF_COOKED_RUNTIME_ONLY)
	ScriptRuntime::ScriptRuntime(const Project& SourceProject, ScriptRuntimeDesc Description, ScriptRuntimeServices Services)
		: m_Project(&SourceProject), m_Description(Description), m_Services(Services)
	{
	}
#endif

	ScriptRuntime::ScriptRuntime(
		std::shared_ptr<const CookedContentProvider> CookedAssets, ScriptRuntimeDesc Description, ScriptRuntimeServices Services)
		: m_Description(Description), m_Services(Services)
	{
		m_Services.CookedAssets = std::move(CookedAssets);
	}

	ScriptRuntime::~ScriptRuntime()
	{
		Stop();
	}

	std::expected<void, ScriptRuntimeError> ScriptRuntime::Start(Scene& Source)
	{
		if (m_Impl) return std::unexpected(MakeRuntimeError(ScriptRuntimeErrorCode::AlreadyRunning, "Script runtime is already started"));
		if (m_Description.MaxSourceBytes == 0 || m_Description.MaxTotalScriptSourceBytes == 0 ||
			m_Description.MaxScriptModuleCount == 0 || m_Description.MaxLuaMemoryBytes < 64u * 1024u ||
			m_Description.MaxTotalLuaMemoryBytes < 64u * 1024u || m_Description.MaxInstructionsPerCallback < InstructionHookQuantum)
		{
			return std::unexpected(MakeRuntimeError(ScriptRuntimeErrorCode::InvalidSettings,
				"Script source, per-state/total memory, and instruction limits must be positive and large enough to initialize Lua"));
		}

		try
		{
			if (!m_Services.CookedAssets
#if !defined(PF_COOKED_RUNTIME_ONLY)
				&& !m_Project
#endif
			)
				return std::unexpected(MakeRuntimeError(ScriptRuntimeErrorCode::InitializationFailed,
					"Script runtime requires a source project or cooked asset provider"));
			auto Candidate = std::make_unique<Impl>(m_Project, m_Services.CookedAssets, m_Description, m_Services);
			Candidate->Source = &Source;
			for (const Entity& EntityValue : Source.GetEntities())
			{
				if (!EntityValue.IsValid()) continue;
				const auto Component = EntityValue.GetScript();
				if (!Component)
				{
					Candidate->RecordDiagnostic(
						MakeDiagnostic(EntityValue.GetUUID(), {}, ScriptDiagnosticCode::InvalidComponent, Component.error().Message));
					continue;
				}
				if (Component->has_value() && Component->value().Enabled) Candidate->Attach(Source, EntityValue, Component->value());
			}
			m_Impl = std::move(Candidate);
			m_StoppedDiagnostics.clear();
			return {};
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(MakeRuntimeError(
				ScriptRuntimeErrorCode::InitializationFailed, std::string("Could not start script runtime: ") + Exception.what()));
		}
	}

	std::expected<uint64_t, ScriptRuntimeError> ScriptRuntime::RequestReload(const Entity& Target)
	{
		if (!m_Impl) return std::unexpected(MakeRuntimeError(ScriptRuntimeErrorCode::NotRunning, "Script runtime is not started"));
		if (!Target.IsValid()) return std::unexpected(MakeRuntimeError(ScriptRuntimeErrorCode::OperationFailed,
			"Script reload requires a valid entity handle"));
		const auto Current = m_Impl->Source->FindEntity(Target.GetUUID());
		if (!Current || *Current != Target)
			return std::unexpected(MakeRuntimeError(ScriptRuntimeErrorCode::DifferentScene,
				"Script reload target does not belong to this runtime's current entity incarnation"));
		const auto Component = Target.GetScript();
		if (!Component || !Component->has_value() || !Component->value().Enabled)
			return std::unexpected(MakeRuntimeError(ScriptRuntimeErrorCode::OperationFailed,
				"Script reload requires an enabled ScriptComponent"));

		std::lock_guard Lock(m_Impl->ReloadMutex);
		if (m_Impl->Stopping) return std::unexpected(MakeRuntimeError(ScriptRuntimeErrorCode::OperationFailed,
			"Script runtime is stopping and cannot accept reload requests"));
		if (m_Impl->NextReloadTicket == 0 || m_Impl->NextReloadTicket == std::numeric_limits<uint64_t>::max())
			return std::unexpected(MakeRuntimeError(ScriptRuntimeErrorCode::OperationFailed,
				"Script reload request identity space is exhausted"));
		const uint64_t Ticket = m_Impl->NextReloadTicket++;
		PendingScriptReload Request{ Ticket, m_Description.SessionGeneration, Component->value().ScriptAsset, {}, Target };
		m_Impl->LatestReloadTickets.insert_or_assign(Target.GetUUID(), Ticket);
		m_Impl->PendingReloads.push_back(std::move(Request));
		return Ticket;
	}

	bool ScriptRuntime::CancelReload(uint64_t Ticket) noexcept
	{
		if (!m_Impl || Ticket == 0) return false;
		try
		{
			std::lock_guard Lock(m_Impl->ReloadMutex);
			const bool Active = m_Impl->ActiveReloadTickets.contains(Ticket);
			const bool Pending = std::any_of(m_Impl->PendingReloads.begin(), m_Impl->PendingReloads.end(),
				[Ticket](const PendingScriptReload& Request) { return Request.Ticket == Ticket; });
			if (!Active && !Pending) return false;
			m_Impl->CancelledReloadTickets.insert(Ticket);
			for (auto It = m_Impl->LatestReloadTickets.begin(); It != m_Impl->LatestReloadTickets.end(); ++It)
			{
				if (It->second == Ticket)
				{
					m_Impl->LatestReloadTickets.erase(It);
					break;
				}
			}
			return true;
		}
		catch (...)
		{
			return false;
		}
	}

	std::expected<size_t, ScriptRuntimeError> ScriptRuntime::ServicePendingReloads(Scene& Source)
	{
		if (!m_Impl) return std::unexpected(MakeRuntimeError(ScriptRuntimeErrorCode::NotRunning, "Script runtime is not started"));
		if (m_Impl->Source != &Source)
			return std::unexpected(MakeRuntimeError(ScriptRuntimeErrorCode::DifferentScene,
				"Script runtime was started for a different Scene"));

		std::deque<PendingScriptReload> Requests;
		{
			std::lock_guard Lock(m_Impl->ReloadMutex);
			Requests.swap(m_Impl->PendingReloads);
		}

		size_t ActivatedCount = 0;
		for (PendingScriptReload& Request : Requests)
		{
			const auto IsCurrentTicket = [&]()
			{
				std::lock_guard Lock(m_Impl->ReloadMutex);
				const auto Latest = m_Impl->LatestReloadTickets.find(Request.Target.GetUUID());
				return !m_Impl->Stopping && Request.SessionGeneration == m_Description.SessionGeneration &&
					!m_Impl->CancelledReloadTickets.contains(Request.Ticket) && Latest != m_Impl->LatestReloadTickets.end() &&
					Latest->second == Request.Ticket;
			};
			const auto FinishTicket = [&]()
			{
				std::lock_guard Lock(m_Impl->ReloadMutex);
				m_Impl->ActiveReloadTickets.erase(Request.Ticket);
				m_Impl->CancelledReloadTickets.erase(Request.Ticket);
				const auto Latest = m_Impl->LatestReloadTickets.find(Request.Target.GetUUID());
				if (Latest != m_Impl->LatestReloadTickets.end() && Latest->second == Request.Ticket)
					m_Impl->LatestReloadTickets.erase(Latest);
			};
			const auto Report = [&](ScriptDiagnosticCode Code, std::string Message)
			{
				m_Impl->RecordDiagnostic(MakeDiagnostic(Request.Target.GetUUID(), Request.ScriptAsset, Code, std::move(Message)));
			};
			const auto RevalidateEntity = [&]() -> std::optional<ScriptComponent>
			{
				const auto Current = Source.FindEntity(Request.Target.GetUUID());
				if (!Current || *Current != Request.Target) return std::nullopt;
				const auto Component = Current->GetScript();
				if (!Component || !Component->has_value() || !Component->value().Enabled ||
					Component->value().ScriptAsset != Request.ScriptAsset)
					return std::nullopt;
				return Component->value();
			};

			{
				std::lock_guard Lock(m_Impl->ReloadMutex);
				const auto Latest = m_Impl->LatestReloadTickets.find(Request.Target.GetUUID());
				if (m_Impl->Stopping || Request.SessionGeneration != m_Description.SessionGeneration ||
					m_Impl->CancelledReloadTickets.contains(Request.Ticket) || Latest == m_Impl->LatestReloadTickets.end() ||
					Latest->second != Request.Ticket)
				{
					m_Impl->CancelledReloadTickets.erase(Request.Ticket);
					continue;
				}
				m_Impl->ActiveReloadTickets.insert(Request.Ticket);
			}

			bool OldTeardownStarted = false;
			bool FailureMarkerPrepared = false;
			try
			{
				auto Component = RevalidateEntity();
				if (!Component)
				{
					Report(ScriptDiagnosticCode::ReloadRejected,
						"Reload request became stale because the entity incarnation or ScriptComponent changed");
					FinishTicket();
					continue;
				}
				auto Snapshot = m_Impl->ResolveSnapshot(Request.ScriptAsset, Request.Target.GetUUID());
				if (!Snapshot)
				{
					m_Impl->RecordDiagnostic(std::move(Snapshot.error()));
					FinishTicket();
					continue;
				}
				Request.SourceFingerprint = (*Snapshot)->Fingerprint;
				auto Candidate = m_Impl->PrepareInstance(Source, Request.Target, *Component, *Snapshot);
				if (!Candidate)
				{
					m_Impl->RecordDiagnostic(std::move(Candidate.error()));
					if (!m_Impl->Scripts.contains(Request.Target.GetUUID()))
						m_Impl->FailedAttachments.insert_or_assign(Request.Target.GetUUID(),
							Impl::FailedAttachment{ Request.Target, Request.ScriptAsset });
					FinishTicket();
					continue;
				}

				auto RecheckedSnapshot = m_Impl->ResolveSnapshot(Request.ScriptAsset, Request.Target.GetUUID());
				if (!RecheckedSnapshot || (*RecheckedSnapshot)->Fingerprint != Request.SourceFingerprint)
				{
					Report(ScriptDiagnosticCode::ReloadRejected, RecheckedSnapshot
						? "Script source changed while its reload candidate was being prepared"
						: "Could not revalidate script source before replacing the active attachment: " + RecheckedSnapshot.error().Message);
					FinishTicket();
					continue;
				}
				if (!IsCurrentTicket() || !RevalidateEntity())
				{
					Report(ScriptDiagnosticCode::ReloadRejected, "Reload request was superseded or became stale before teardown");
					FinishTicket();
					continue;
				}

				auto Existing = m_Impl->Scripts.find(Request.Target.GetUUID());
				if (Existing != m_Impl->Scripts.end())
				{
					if (Existing->second->BoundEntity != Request.Target || Existing->second->Asset != Request.ScriptAsset)
					{
						Report(ScriptDiagnosticCode::ReloadRejected, "Active attachment no longer matches the requested entity incarnation and asset");
						FinishTicket();
						continue;
					}
					// Allocate the disabled-attachment marker before invoking user teardown. If OnDestroy
					// fails or throws, SynchronizeAttachments must not silently recreate the old script.
					m_Impl->FailedAttachments.insert_or_assign(Request.Target.GetUUID(),
						Impl::FailedAttachment{ Request.Target, Request.ScriptAsset });
					FailureMarkerPrepared = true;
					OldTeardownStarted = true;
					auto Destroyed = m_Impl->InvokeDestroy(*Existing->second);
					m_Impl->Scripts.erase(Existing);
					if (!Destroyed)
					{
						Report(ScriptDiagnosticCode::ReloadActivationFailed,
							"Old script teardown failed; the attachment was disabled: " + Destroyed.error());
						FinishTicket();
						continue;
					}
				}

				if (!IsCurrentTicket() || !RevalidateEntity())
				{
					if (OldTeardownStarted) Report(ScriptDiagnosticCode::ReloadActivationFailed,
						"Reload became stale after old OnDestroy began; the attachment remains disabled");
					FinishTicket();
					continue;
				}
				auto FinalSnapshot = m_Impl->ResolveSnapshot(Request.ScriptAsset, Request.Target.GetUUID());
				if (!FinalSnapshot || (*FinalSnapshot)->Fingerprint != Request.SourceFingerprint)
				{
					Report(ScriptDiagnosticCode::ReloadActivationFailed, FinalSnapshot
						? "Script source changed after old OnDestroy began; the attachment remains disabled"
						: "Could not revalidate script source after old OnDestroy began; the attachment remains disabled: " +
							FinalSnapshot.error().Message);
					FinishTicket();
					continue;
				}

				auto LatestComponent = RevalidateEntity();
				if (!LatestComponent)
				{
					Report(ScriptDiagnosticCode::ReloadActivationFailed,
						"Entity or ScriptComponent changed after old OnDestroy began; the attachment remains disabled");
					FinishTicket();
					continue;
				}
				if (!OldTeardownStarted)
				{
					// There is no previous attachment to retire, but a failed activation must still
					// remain disabled instead of being retried implicitly on the next synchronization.
					m_Impl->FailedAttachments.insert_or_assign(Request.Target.GetUUID(),
						Impl::FailedAttachment{ Request.Target, Request.ScriptAsset });
					FailureMarkerPrepared = true;
				}
				if (auto Activated = m_Impl->ActivateInstance(**Candidate); !Activated)
				{
					m_Impl->RecordPendingModuleFailure(**Candidate);
					m_Impl->RecordDiagnostic(MakeScriptDiagnostic(Request.Target.GetUUID(), Request.ScriptAsset,
						ScriptDiagnosticCode::ReloadActivationFailed,
						"Replacement script activation failed; the attachment was disabled: " + Activated.error(),
						*(*Candidate)->SourceSnapshot, (*Candidate)->SourceSnapshot->MainSourcePath));
					FinishTicket();
					continue;
				}
				m_Impl->Scripts.insert_or_assign(Request.Target.GetUUID(), std::move(*Candidate));
				m_Impl->FailedAttachments.erase(Request.Target.GetUUID());
				Observe(m_Impl->Services.Observer,
					{ .Kind = ScriptRuntimeObservationKind::ScriptAttached, .Entity = Request.Target.GetUUID(),
						.ScriptAsset = Request.ScriptAsset });
				++ActivatedCount;
				FinishTicket();
			}
			catch (const std::exception& Exception)
			{
				if (OldTeardownStarted)
				{
					m_Impl->Scripts.erase(Request.Target.GetUUID());
				}
				Report(FailureMarkerPrepared ? ScriptDiagnosticCode::ReloadActivationFailed : ScriptDiagnosticCode::ReloadRejected,
					std::string(OldTeardownStarted ? "Reload failed after old attachment teardown; attachment disabled: "
								 : FailureMarkerPrepared ? "Reload activation failed; attachment disabled: "
								 : "Reload preparation failed; old attachment retained: ") + Exception.what());
				FinishTicket();
			}
			catch (...)
			{
				if (OldTeardownStarted)
				{
					m_Impl->Scripts.erase(Request.Target.GetUUID());
				}
				Report(FailureMarkerPrepared ? ScriptDiagnosticCode::ReloadActivationFailed : ScriptDiagnosticCode::ReloadRejected,
					OldTeardownStarted ? "Reload failed after old attachment teardown; attachment disabled"
								 : FailureMarkerPrepared ? "Reload activation failed; attachment disabled"
								 : "Reload preparation failed; old attachment retained");
				FinishTicket();
			}
		}
		return ActivatedCount;
	}

	void ScriptRuntime::Stop() noexcept
	{
		if (!m_Impl) return;
		{
			std::lock_guard Lock(m_Impl->ReloadMutex);
			m_Impl->Stopping = true;
			m_Impl->PendingReloads.clear();
			m_Impl->LatestReloadTickets.clear();
			m_Impl->CancelledReloadTickets.clear();
		}
		try
		{
			for (auto& [Identifier, Instance] : m_Impl->Scripts)
			{
				(void)Identifier;
				(void)m_Impl->InvokeDestroy(*Instance);
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
		return m_Impl ? std::span<const ScriptDiagnostic>(m_Impl->Diagnostics) : std::span<const ScriptDiagnostic>(m_StoppedDiagnostics);
	}

	void ScriptRuntime::ClearDiagnostics() noexcept
	{
		if (m_Impl) m_Impl->Diagnostics.clear();
		m_StoppedDiagnostics.clear();
	}

	std::expected<void, ScriptRuntimeError> ScriptRuntime::Advance(Scene& Source, Timestep DeltaTime)
	{
		if (!m_Impl) return std::unexpected(MakeRuntimeError(ScriptRuntimeErrorCode::NotRunning, "Script runtime is not started"));
		if (m_Impl->Source != &Source)
			return std::unexpected(
				MakeRuntimeError(ScriptRuntimeErrorCode::DifferentScene, "Script runtime was started for a different Scene"));
		const double DeltaSeconds = DeltaTime.GetSeconds();
		if (!std::isfinite(DeltaSeconds) || DeltaSeconds < 0.0)
			return std::unexpected(
				MakeRuntimeError(ScriptRuntimeErrorCode::InvalidDeltaTime, "Script delta time must be finite and non-negative"));

		try
		{
			m_Impl->SynchronizeAttachments(Source);
			for (auto Iterator = m_Impl->Scripts.begin(); Iterator != m_Impl->Scripts.end();)
			{
				ScriptInstance& Instance = *Iterator->second;
				const auto CurrentEntity = Source.FindEntity(Iterator->first);
				bool Eligible = CurrentEntity && *CurrentEntity == Instance.BoundEntity;
				if (Eligible)
				{
					const auto Component = CurrentEntity->GetScript();
					if (!Component)
					{
						m_Impl->RecordDiagnostic(MakeDiagnostic(
							Iterator->first, Instance.Asset, ScriptDiagnosticCode::InvalidComponent, Component.error().Message));
						Eligible = false;
					}
					else
					{
						Eligible = Component->has_value() && Component->value().Enabled && Component->value().ScriptAsset == Instance.Asset;
					}
				}

				if (!Eligible)
				{
					(void)m_Impl->InvokeDestroy(Instance);
					Iterator = m_Impl->Scripts.erase(Iterator);
					continue;
				}
				if (Instance.Faulted)
				{
					++Iterator;
					continue;
				}
				if (auto UpdateResult = RunCallback(Instance, 1, DeltaSeconds); !UpdateResult)
				{
					Instance.Faulted = true;
					m_Impl->RecordDiagnostic(MakeScriptDiagnostic(Instance.BoundEntity.GetUUID(), Instance.Asset,
						ScriptDiagnosticCode::UpdateFailed, std::move(UpdateResult.error()), *Instance.SourceSnapshot,
						Instance.SourceSnapshot->MainSourcePath));
				}
				m_Impl->RecordPendingModuleFailure(Instance);
				++Iterator;
			}
			return {};
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(MakeRuntimeError(
				ScriptRuntimeErrorCode::OperationFailed, std::string("Could not advance script runtime: ") + Exception.what()));
		}
	}

	bool ScriptRuntime::DispatchPhysicsContact(Scene& Source, const PhysicsContactEvent& Event) noexcept
	{
		if (!m_Impl || m_Impl->Source != &Source || Event.SessionGeneration != m_Description.SessionGeneration)
			return true;
		size_t CallbackIndex = 0;
		if (Event.IsSensorPair)
		{
			CallbackIndex = Event.Phase == PhysicsContactPhase::Begin ? 6 :
				Event.Phase == PhysicsContactPhase::Stay ? 7 : 8;
		}
		else
		{
			CallbackIndex = Event.Phase == PhysicsContactPhase::Begin ? 3 :
				Event.Phase == PhysicsContactPhase::Stay ? 4 : 5;
		}

		try
		{
			const auto DispatchTo = [&](const Entity& Recipient, const Entity& Other) -> bool
			{
				const auto Current = Source.FindEntity(Recipient.GetUUID());
				if (!Current || *Current != Recipient) return true;
				const auto Script = m_Impl->Scripts.find(Recipient.GetUUID());
				if (Script == m_Impl->Scripts.end()) return true;
				ScriptInstance& Instance = *Script->second;
				if (Instance.BoundEntity != Recipient || Instance.Faulted || Instance.Destroying || !Instance.SourceSnapshot)
					return true;
				Instance.DeferredRuntimeTransitionRequested = false;
				auto Result = RunContactCallback(Instance, CallbackIndex, Other);
				if (!Result)
				{
					Instance.Faulted = true;
					m_Impl->RecordDiagnostic(MakeScriptDiagnostic(Recipient.GetUUID(), Instance.Asset,
						ScriptDiagnosticCode::UpdateFailed, std::move(Result.error()), *Instance.SourceSnapshot,
						Instance.SourceSnapshot->MainSourcePath));
				}
				m_Impl->RecordPendingModuleFailure(Instance);
				return !Instance.DeferredRuntimeTransitionRequested;
			};

			if (!DispatchTo(Event.First, Event.Second)) return false;
			return DispatchTo(Event.Second, Event.First);
		}
		catch (...)
		{
			return false;
		}
	}

	bool ScriptRuntime::DispatchRuntimeUIAction(Scene& Source, const Entity& Recipient, std::string_view Action) noexcept
	{
		if (!m_Impl || m_Impl->Source != &Source || Action.empty() || Action.size() > 128 ||
			Action.find_first_of("\r\n") != std::string_view::npos || Action.find('\0') != std::string_view::npos)
			return true;
		try
		{
			const auto Current = Source.FindEntity(Recipient.GetUUID());
			if (!Current || *Current != Recipient) return true;
			const auto Found = m_Impl->Scripts.find(Recipient.GetUUID());
			if (Found == m_Impl->Scripts.end()) return true;
			ScriptInstance& Instance = *Found->second;
			if (Instance.BoundEntity != Recipient || Instance.Faulted || Instance.Destroying || !Instance.SourceSnapshot) return true;
			Instance.DeferredRuntimeTransitionRequested = false;
			auto Result = RunUIActionCallback(Instance, Action);
			if (!Result)
			{
				Instance.Faulted = true;
				m_Impl->RecordDiagnostic(MakeScriptDiagnostic(Recipient.GetUUID(), Instance.Asset,
					ScriptDiagnosticCode::UpdateFailed, std::move(Result.error()), *Instance.SourceSnapshot,
					Instance.SourceSnapshot->MainSourcePath));
			}
			m_Impl->RecordPendingModuleFailure(Instance);
			return !Instance.DeferredRuntimeTransitionRequested;
		}
		catch (...)
		{
			return false;
		}
	}
} // namespace PulseForge
