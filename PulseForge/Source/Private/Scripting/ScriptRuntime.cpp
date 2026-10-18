#include "Scripting/ScriptRuntime.h"
#include "Scripting/ScriptRuntimeInternal.h"
#include "Scripting/ScriptEnvironment.h"

#include "Assets/AssetPathResolver.h"
#include "Assets/AssetTypes.h"
#include "Assets/Project.h"
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
	using ScriptRuntimeDetail::CallbackNames;
	using ScriptRuntimeDetail::GetScriptInstance;
	using ScriptRuntimeDetail::InstructionHookQuantum;
	using ScriptRuntimeDetail::LuaMemory;
	using ScriptRuntimeDetail::LuaMemoryPool;
	using ScriptRuntimeDetail::ScriptInstance;

	namespace
	{
		ScriptRuntimeError MakeRuntimeError(ScriptRuntimeErrorCode Code, std::string Message)
		{
			return { Code, std::move(Message) };
		}

		ScriptDiagnostic MakeDiagnostic(UUID EntityIdentifier, AssetID ScriptAsset, ScriptDiagnosticCode Code, std::string Message)
		{
			return { EntityIdentifier, ScriptAsset, Code, std::move(Message) };
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
			const char* Message = lua_tostring(State, -1);
			std::string Result = Message ? Message : "Lua returned a non-string error object";
			lua_pop(State, 1);
			return Result;
		}

		std::expected<void, std::string> RunCallback(
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
				return {};
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
			const int Status = lua_pcall(State, ArgumentCount, 0, 0);
			lua_sethook(State, nullptr, 0, 0);
			if (Status != LUA_OK) return std::unexpected(LuaErrorMessage(State));
			return {};
		}

		std::expected<std::string, ScriptDiagnostic> ReadScriptSource(
			const Project& SourceProject, const AssetID& ScriptAsset, UUID EntityIdentifier, size_t MaximumSourceBytes)
		{
			const auto Record = SourceProject.GetAssetRegistry().Find(ScriptAsset);
			if (!Record)
			{
				return std::unexpected(MakeDiagnostic(EntityIdentifier, ScriptAsset, ScriptDiagnosticCode::AssetNotFound,
					"Script UUID is not present in the project asset registry"));
			}
			if (!HasAssetTypeExtension(Record->ProjectRelativePath, AssetFileType::Script))
			{
				return std::unexpected(MakeDiagnostic(
					EntityIdentifier, ScriptAsset, ScriptDiagnosticCode::WrongAssetType, "Script asset must reference a .lua source file"));
			}

			const auto ResolvedPath = AssetPathResolver::ResolveManagedSourcePath(SourceProject.GetRootPath(), Record->ProjectRelativePath);
			if (!ResolvedPath)
			{
				return std::unexpected(
					MakeDiagnostic(EntityIdentifier, ScriptAsset, ScriptDiagnosticCode::FileReadFailed, ResolvedPath.error().Message));
			}

			try
			{
				std::error_code FileError;
				const uintmax_t FileSize = std::filesystem::file_size(*ResolvedPath, FileError);
				if (FileError || FileSize > MaximumSourceBytes || FileSize > static_cast<uintmax_t>(std::numeric_limits<size_t>::max()) ||
					FileSize > static_cast<uintmax_t>(std::numeric_limits<std::streamsize>::max()))
				{
					return std::unexpected(MakeDiagnostic(EntityIdentifier, ScriptAsset, ScriptDiagnosticCode::FileReadFailed,
						FileError ? "Could not determine script source size: " + FileError.message()
								  : "Script source exceeds the configured size limit"));
				}

				std::ifstream Input(*ResolvedPath, std::ios::binary);
				if (!Input.is_open())
					return std::unexpected(MakeDiagnostic(EntityIdentifier, ScriptAsset, ScriptDiagnosticCode::FileReadFailed,
						"Could not open the managed script source file"));

				std::string Source(static_cast<size_t>(FileSize), '\0');
				if (!Source.empty())
				{
					Input.read(Source.data(), static_cast<std::streamsize>(Source.size()));
					if (!Input || Input.bad())
						return std::unexpected(MakeDiagnostic(EntityIdentifier, ScriptAsset, ScriptDiagnosticCode::FileReadFailed,
							"Could not read the complete managed script source file"));
				}
				return Source;
			}
			catch (const std::exception& Exception)
			{
				return std::unexpected(MakeDiagnostic(EntityIdentifier, ScriptAsset, ScriptDiagnosticCode::FileReadFailed,
					std::string("Could not read script source: ") + Exception.what()));
			}
		}
	} // namespace

	struct ScriptRuntime::Impl final
	{
		const Project& SourceProject;
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

		Impl(const Project& ProjectValue, ScriptRuntimeDesc RuntimeDescription, ScriptRuntimeServices RuntimeServices)
			: SourceProject(ProjectValue), Description(RuntimeDescription), Services(RuntimeServices)
		{
			MemoryPool.MaximumBytes = Description.MaxTotalLuaMemoryBytes;
		}

		void RecordDiagnostic(ScriptDiagnostic Diagnostic)
		{
			PF_ERROR("Script on entity {0} (asset {1}) failed: {2}", Diagnostic.Entity.ToString(), Diagnostic.ScriptAsset.ToString(),
				Diagnostic.Message);
			Diagnostics.push_back(std::move(Diagnostic));
		}

		std::expected<std::unique_ptr<ScriptInstance>, ScriptDiagnostic> CreateInstance(
			Scene& SceneValue, const Entity& EntityValue, const ScriptComponent& Component)
		{
			const UUID EntityIdentifier = EntityValue.GetUUID();
			if (auto Validation = Component.Validate(); !Validation)
			{
				return std::unexpected(MakeDiagnostic(
					EntityIdentifier, Component.ScriptAsset, ScriptDiagnosticCode::InvalidComponent, Validation.error().Message));
			}

			auto LoadedSource = ReadScriptSource(SourceProject, Component.ScriptAsset, EntityIdentifier, Description.MaxSourceBytes);
			if (!LoadedSource) return std::unexpected(std::move(LoadedSource.error()));
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
				return std::unexpected(MakeDiagnostic(EntityIdentifier, Component.ScriptAsset,
					ScriptDiagnosticCode::LuaInitializationFailed, "Could not create a Lua state within the configured memory limit"));
			}
			*static_cast<ScriptInstance**>(lua_getextraspace(Instance->State)) = Instance.get();

			lua_pushcfunction(Instance->State, ScriptEnvironment::OpenRestrictedLibraries);
			if (lua_pcall(Instance->State, 0, 0, 0) != LUA_OK)
			{
				return std::unexpected(MakeDiagnostic(EntityIdentifier, Component.ScriptAsset,
					ScriptDiagnosticCode::LuaInitializationFailed, LuaErrorMessage(Instance->State)));
			}

			lua_pushcfunction(Instance->State, ScriptEnvironment::Initialize);
			if (lua_pcall(Instance->State, 0, 0, 0) != LUA_OK)
			{
				return std::unexpected(MakeDiagnostic(EntityIdentifier, Component.ScriptAsset,
					ScriptDiagnosticCode::LuaInitializationFailed, LuaErrorMessage(Instance->State)));
			}

			const auto AssetRecord = SourceProject.GetAssetRegistry().Find(Component.ScriptAsset);
			const std::string ChunkName = AssetRecord ? "@" + AssetRecord->ProjectRelativePath.generic_string() : "@PulseForge script";
			const int LoadStatus = luaL_loadbufferx(Instance->State, ScriptSource.data(), ScriptSource.size(), ChunkName.c_str(), "t");
			if (LoadStatus != LUA_OK)
			{
				return std::unexpected(MakeDiagnostic(
					EntityIdentifier, Component.ScriptAsset, ScriptDiagnosticCode::CompileFailed, LuaErrorMessage(Instance->State)));
			}

			Instance->InstructionBudget = (Description.MaxInstructionsPerCallback / InstructionHookQuantum) * InstructionHookQuantum;
			lua_sethook(Instance->State, InstructionBudgetHook, LUA_MASKCOUNT, InstructionHookQuantum);
			const int ExecuteStatus = lua_pcall(Instance->State, 0, 0, 0);
			lua_sethook(Instance->State, nullptr, 0, 0);
			if (ExecuteStatus != LUA_OK)
			{
				return std::unexpected(MakeDiagnostic(
					EntityIdentifier, Component.ScriptAsset, ScriptDiagnosticCode::InitializationFailed, LuaErrorMessage(Instance->State)));
			}

			if (auto CreateResult = RunCallback(*Instance, 0); !CreateResult)
			{
				return std::unexpected(MakeDiagnostic(
					EntityIdentifier, Component.ScriptAsset, ScriptDiagnosticCode::InitializationFailed, std::move(CreateResult.error())));
			}
			return Instance;
		}

		void InvokeDestroy(ScriptInstance& Instance)
		{
			if (Instance.DestroyInvoked) return;
			Instance.DestroyInvoked = true;
			Instance.Destroying = true;
			struct DestroyingReset final
			{
				ScriptInstance& Instance;
				~DestroyingReset() { Instance.Destroying = false; }
			} Reset{ Instance };

			if (auto DestroyResult = RunCallback(Instance, 2); !DestroyResult)
			{
				RecordDiagnostic(MakeDiagnostic(
					Instance.BoundEntity.GetUUID(), Instance.Asset, ScriptDiagnosticCode::DestroyFailed, std::move(DestroyResult.error())));
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
					FailedAttachments.insert_or_assign(EntityValue.GetUUID(), FailedAttachment{ EntityValue, Component.ScriptAsset });
					return;
				}
				Scripts.insert_or_assign(EntityValue.GetUUID(), std::move(*Instance));
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

				InvokeDestroy(*Iterator->second);
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

	ScriptRuntime::ScriptRuntime(const Project& SourceProject, ScriptRuntimeDesc Description, ScriptRuntimeServices Services)
		: m_Project(SourceProject), m_Description(Description), m_Services(Services)
	{
	}

	ScriptRuntime::~ScriptRuntime()
	{
		Stop();
	}

	std::expected<void, ScriptRuntimeError> ScriptRuntime::Start(Scene& Source)
	{
		if (m_Impl) return std::unexpected(MakeRuntimeError(ScriptRuntimeErrorCode::AlreadyRunning, "Script runtime is already started"));
		if (m_Description.MaxSourceBytes == 0 || m_Description.MaxLuaMemoryBytes < 64u * 1024u ||
			m_Description.MaxTotalLuaMemoryBytes < 64u * 1024u || m_Description.MaxInstructionsPerCallback < InstructionHookQuantum)
		{
			return std::unexpected(MakeRuntimeError(ScriptRuntimeErrorCode::InvalidSettings,
				"Script source, per-state/total memory, and instruction limits must be positive and large enough to initialize Lua"));
		}

		try
		{
			auto Candidate = std::make_unique<Impl>(m_Project, m_Description, m_Services);
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

	void ScriptRuntime::Stop() noexcept
	{
		if (!m_Impl) return;
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
					m_Impl->InvokeDestroy(Instance);
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
					m_Impl->RecordDiagnostic(MakeDiagnostic(Instance.BoundEntity.GetUUID(), Instance.Asset,
						ScriptDiagnosticCode::UpdateFailed, std::move(UpdateResult.error())));
				}
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
} // namespace PulseForge
