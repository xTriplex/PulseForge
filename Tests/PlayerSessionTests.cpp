#include "TestSupport.h"

#include "Assets/AssetTypes.h"
#include "Assets/AssetOperations.h"
#include "Assets/Project.h"
#include "Assets/SceneAssetService.h"
#include "Assets/CookedAssetCatalog.h"
#include "Assets/CookedProductCodec.h"
#include "Runtime/CookedContentProvider.h"
#include "Runtime/PlayerSession.h"
#include "Scene/Components/AudioSourceComponent.h"
#include "Scene/Components/BlueprintComponent.h"
#include "Scene/Components/BoxColliderComponent.h"
#include "Scene/Components/MeshRendererComponent.h"
#include "Scene/Components/RigidbodyComponent.h"
#include "Scene/SceneSerializer.h"
#include "Scene/Components/ScriptComponent.h"
#include "Scene/Scene.h"
#include "Scripting/BlueprintAsset.h"
#include "Scripting/ScriptRuntime.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
	std::vector<std::byte> MakeCookedSilentWave()
	{
		// One mono PCM16 sample at 44.1 kHz is sufficient to exercise the production decoder.
		constexpr uint32_t DataSize = 2;
		std::vector<std::byte> Data(44 + DataSize);
		auto WriteFourCC = [&Data](size_t Offset, std::string_view Value)
		{
			for (size_t Index = 0; Index < Value.size(); ++Index)
				Data[Offset + Index] = static_cast<std::byte>(static_cast<unsigned char>(Value[Index]));
		};
		auto Write16 = [&Data](size_t Offset, uint16_t Value)
		{
			Data[Offset] = static_cast<std::byte>(Value & 0xff);
			Data[Offset + 1] = static_cast<std::byte>((Value >> 8) & 0xff);
		};
		auto Write32 = [&Data](size_t Offset, uint32_t Value)
		{
			for (size_t Byte = 0; Byte < 4; ++Byte)
				Data[Offset + Byte] = static_cast<std::byte>((Value >> (Byte * 8)) & 0xff);
		};

		WriteFourCC(0, "RIFF");
		Write32(4, static_cast<uint32_t>(Data.size() - 8));
		WriteFourCC(8, "WAVE");
		WriteFourCC(12, "fmt ");
		Write32(16, 16);
		Write16(20, 1);
		Write16(22, 1);
		Write32(24, 44100);
		Write32(28, 88200);
		Write16(32, 2);
		Write16(34, 16);
		WriteFourCC(36, "data");
		Write32(40, DataSize);
		return Data;
	}

	struct ScriptObservationBuffer
	{
		std::array<PulseForge::ScriptRuntimeObservation, 512> Events{};
		size_t Count = 0;
		bool Overflowed = false;
	};

	void StoreScriptObservation(void* Context, const PulseForge::ScriptRuntimeObservation& Observation) noexcept
	{
		auto& Buffer = *static_cast<ScriptObservationBuffer*>(Context);
		if (Buffer.Count == Buffer.Events.size())
		{
			Buffer.Overflowed = true;
			return;
		}
		Buffer.Events[Buffer.Count++] = Observation;
	}

	size_t CountScriptObservations(
		const ScriptObservationBuffer& Buffer,
		PulseForge::ScriptRuntimeObservationKind Kind,
		const PulseForge::AssetID& ScriptAsset,
		PulseForge::UUID Entity,
		std::optional<PulseForge::ScriptRuntimeCallback> Callback = std::nullopt)
	{
		size_t Result = 0;
		for (size_t Index = 0; Index < Buffer.Count; ++Index)
		{
			const auto& Event = Buffer.Events[Index];
			if (Event.Kind == Kind && Event.ScriptAsset == ScriptAsset && Event.Entity == Entity &&
				(!Callback || Event.Callback == *Callback))
			{
				++Result;
			}
		}
		return Result;
	}

	std::string ReadFile(const std::filesystem::path& Path)
	{
		std::ifstream Input(Path, std::ios::binary);
		return { std::istreambuf_iterator<char>(Input), std::istreambuf_iterator<char>() };
	}

	void TestPlayerSessionLifecycleAndTransitions(TestRunner& Tests)
	{
		using namespace PulseForge;
		const auto Identifier = UUID::Generate();
		PF_CHECK(Tests, Identifier.has_value());
		if (!Identifier) return;

		std::error_code Error;
		const auto Root = std::filesystem::temp_directory_path(Error) / ("PulseForge Player Session " + Identifier->ToString());
		PF_CHECK(Tests, !Error);
		if (Error) return;
		struct DirectoryCleanup
		{
			std::filesystem::path Path;
			~DirectoryCleanup()
			{
				std::error_code CleanupError;
				std::filesystem::remove_all(Path, CleanupError);
			}
		} Cleanup{ Root };

		const auto ProjectFile = Root / "Player Project.pfproj";
		auto CreatedProjectResult = Project::Create(ProjectFile, "Player Project");
		PF_CHECK(Tests, CreatedProjectResult.has_value());
		if (!CreatedProjectResult) return;
		std::optional<Project> CreatedProject(std::move(*CreatedProjectResult));
		std::filesystem::create_directories(Root / "Assets" / "Scripts", Error);
		PF_CHECK(Tests, !Error);
		const std::string TimeScaleScript = "function OnUpdate(dt) assert(entity:set_translation(dt, 0, 0)) end\n"
			"function OnDestroy()\n"
			"  local marker, message = scene.create_entity('original session teardown marker')\n"
			"  assert(marker, message)\n"
			"end\n";
		const auto ScriptAsset = AssetOperations::CreateAssetFromBytes(CreatedProject->GetAssetRegistry(), CreatedProject->GetRootPath(),
			std::as_bytes(std::span(TimeScaleScript.data(), TimeScaleScript.size())), "Assets/Scripts/time_scale.lua");
		PF_CHECK(Tests, ScriptAsset.has_value());
		if (!ScriptAsset) return;
		const AssetID MissingTransitionAsset{ 0x7600000000000000ull, 0xdeadbeefull };
		const std::string MissingTransitionText = MissingTransitionAsset.ToString();
		const std::string TeardownScript = "function OnCreate()\n"
			"  local marker, message = scene.create_entity('rejected candidate initialization marker')\n"
			"  assert(marker, message)\n"
			"end\n"
			"function OnDestroy()\n"
			"  local marker, message = scene.create_entity('rejected candidate teardown marker')\n"
			"  assert(marker, message)\n"
			"end\n";
		const auto TeardownScriptAsset = AssetOperations::CreateAssetFromBytes(CreatedProject->GetAssetRegistry(), CreatedProject->GetRootPath(),
			std::as_bytes(std::span(TeardownScript.data(), TeardownScript.size())), "Assets/Scripts/teardown.lua");
		PF_CHECK(Tests, TeardownScriptAsset.has_value());
		if (!TeardownScriptAsset) return;
		const std::string RetryScript = "function OnCreate()\n"
			"  local marker, message = scene.create_entity('retry candidate initialization marker')\n"
			"  assert(marker, message)\n"
			"end\n"
			"function OnDestroy()\n"
			"  local marker, message = scene.create_entity('retry candidate teardown marker')\n"
			"  assert(marker, message)\n"
			"end\n";
		const auto RetryScriptAsset = AssetOperations::CreateAssetFromBytes(CreatedProject->GetAssetRegistry(), CreatedProject->GetRootPath(),
			std::as_bytes(std::span(RetryScript.data(), RetryScript.size())), "Assets/Scripts/retry_teardown.lua");
		PF_CHECK(Tests, RetryScriptAsset.has_value());
		if (!RetryScriptAsset) return;
		const std::string CandidateRequestScript = "function OnCreate()\n"
			"  local queued, message = scene.request_transition('" + MissingTransitionText + "')\n"
			"  assert(queued, message)\n"
			"end\n";
		const auto CandidateRequestScriptAsset = AssetOperations::CreateAssetFromBytes(
			CreatedProject->GetAssetRegistry(), CreatedProject->GetRootPath(),
			std::as_bytes(std::span(CandidateRequestScript.data(), CandidateRequestScript.size())),
			"Assets/Scripts/candidate_request.lua");
		PF_CHECK(Tests, CandidateRequestScriptAsset.has_value());
		if (!CandidateRequestScriptAsset) return;
		const std::string TeardownRequestScript = "function OnDestroy()\n"
			"  local queued, message = scene.request_stop()\n"
			"  assert(queued, message)\n"
			"end\n";
		const auto TeardownRequestScriptAsset = AssetOperations::CreateAssetFromBytes(
			CreatedProject->GetAssetRegistry(), CreatedProject->GetRootPath(),
			std::as_bytes(std::span(TeardownRequestScript.data(), TeardownRequestScript.size())),
			"Assets/Scripts/teardown_request.lua");
		PF_CHECK(Tests, TeardownRequestScriptAsset.has_value());
		if (!TeardownRequestScriptAsset) return;

		Scene StartupScene;
		auto StartupEntity = StartupScene.CreateEntity("Startup Entity");
		PF_CHECK(Tests, StartupEntity.has_value());
		if (!StartupEntity) return;
		PF_CHECK(Tests, StartupEntity->SetScript(ScriptComponent{ ScriptAsset->ID }).has_value());
		auto StartupAsset = SceneAssetService::Create(
			CreatedProject->GetAssetRegistry(), CreatedProject->GetRootPath(), "Assets/Startup.scene", StartupScene);
		PF_CHECK(Tests, StartupAsset.has_value());
		if (!StartupAsset) return;
		PF_CHECK(Tests, CreatedProject->SetStartScene(StartupAsset->ID).has_value());

		Scene SecondScene;
		auto SecondEntity = SecondScene.CreateEntity("Second Entity");
		PF_CHECK(Tests, SecondEntity.has_value());
		if (!SecondEntity) return;
		PF_CHECK(Tests, SecondEntity->SetScript(ScriptComponent{ TeardownScriptAsset->ID }).has_value());
		auto SecondAsset = SceneAssetService::Create(
			CreatedProject->GetAssetRegistry(), CreatedProject->GetRootPath(), "Assets/Second.scene", SecondScene);
		PF_CHECK(Tests, SecondAsset.has_value());
		if (!SecondAsset) return;
		Scene RetryScene;
		auto RetryEntity = RetryScene.CreateEntity("Retry Entity");
		PF_CHECK(Tests, RetryEntity.has_value());
		if (!RetryEntity) return;
		PF_CHECK(Tests, RetryEntity->SetScript(ScriptComponent{ RetryScriptAsset->ID }).has_value());
		auto CandidateRequestEntity = RetryScene.CreateEntity("Candidate request isolation probe");
		PF_CHECK(Tests, CandidateRequestEntity.has_value());
		if (!CandidateRequestEntity) return;
		PF_CHECK(Tests, CandidateRequestEntity->SetScript(ScriptComponent{ CandidateRequestScriptAsset->ID }).has_value());
		auto RetryAsset = SceneAssetService::Create(
			CreatedProject->GetAssetRegistry(), CreatedProject->GetRootPath(), "Assets/Retry.scene", RetryScene);
		PF_CHECK(Tests, RetryAsset.has_value());
		if (!RetryAsset) return;
		Scene InvalidPhysicsTransitionScene;
		const auto InvalidPhysicsEntity = InvalidPhysicsTransitionScene.CreateEntity("Invalid destination body");
		PF_CHECK(Tests, InvalidPhysicsEntity.has_value());
		if (!InvalidPhysicsEntity) return;
		PF_CHECK(Tests, InvalidPhysicsEntity->SetRigidbody(RigidbodyComponent{}).has_value());
		auto InvalidPhysicsTransitionAsset = SceneAssetService::Create(
			CreatedProject->GetAssetRegistry(), CreatedProject->GetRootPath(),
			"Assets/InvalidPhysicsTransition.scene", InvalidPhysicsTransitionScene);
		PF_CHECK(Tests, InvalidPhysicsTransitionAsset.has_value());
		if (!InvalidPhysicsTransitionAsset) return;
		const std::string InvalidPhysicsTransitionText = InvalidPhysicsTransitionAsset->ID.ToString();
		const std::string DeferredTransitionScript = "local updates = 0\n"
			"function OnUpdate()\n"
			"  updates = updates + 1\n"
			"  assert(entity:set_translation(updates, 10, 0))\n"
			"  if updates == 1 then\n"
			"    local queued, message = scene.request_transition('" + MissingTransitionText + "')\n"
			"    assert(queued, message)\n"
			"  elseif updates == 2 then\n"
			"    local queued, message = scene.request_transition('" + InvalidPhysicsTransitionText + "')\n"
			"    assert(queued, message)\n"
			"  end\n"
			"end\n";
		const auto DeferredTransitionScriptAsset = AssetOperations::CreateAssetFromBytes(
			CreatedProject->GetAssetRegistry(), CreatedProject->GetRootPath(),
			std::as_bytes(std::span(DeferredTransitionScript.data(), DeferredTransitionScript.size())),
			"Assets/Scripts/deferred_transition.lua");
		PF_CHECK(Tests, DeferredTransitionScriptAsset.has_value());
		if (!DeferredTransitionScriptAsset) return;
		Scene DeferredTransitionScene;
		auto DeferredTransitionEntity = DeferredTransitionScene.CreateEntity("Deferred transition source");
		PF_CHECK(Tests, DeferredTransitionEntity.has_value());
		if (!DeferredTransitionEntity) return;
		PF_CHECK(Tests, DeferredTransitionEntity->SetScript(ScriptComponent{ DeferredTransitionScriptAsset->ID }).has_value());
		auto TeardownRequestEntity = DeferredTransitionScene.CreateEntity("Old teardown request isolation probe");
		PF_CHECK(Tests, TeardownRequestEntity.has_value());
		if (!TeardownRequestEntity) return;
		PF_CHECK(Tests, TeardownRequestEntity->SetScript(ScriptComponent{ TeardownRequestScriptAsset->ID }).has_value());
		auto DeferredPhysicsEntity = DeferredTransitionScene.CreateEntity("Deferred transition physics witness");
		PF_CHECK(Tests, DeferredPhysicsEntity.has_value());
		if (!DeferredPhysicsEntity) return;
		TransformComponent DeferredPhysicsTransform;
		DeferredPhysicsTransform.Translation.y = 10.0f;
		PF_CHECK(Tests, DeferredPhysicsEntity->SetTransform(DeferredPhysicsTransform).has_value());
		PF_CHECK(Tests, DeferredPhysicsEntity->SetRigidbody(
			{ RigidbodyMotionType::Dynamic, 1.0f, 0.5f, 0.0f, false }).has_value());
		PF_CHECK(Tests, DeferredPhysicsEntity->SetBoxCollider(BoxColliderComponent{}).has_value());
		auto DeferredTransitionAsset = SceneAssetService::Create(
			CreatedProject->GetAssetRegistry(), CreatedProject->GetRootPath(),
			"Assets/DeferredTransition.scene", DeferredTransitionScene);
		PF_CHECK(Tests, DeferredTransitionAsset.has_value());
		if (!DeferredTransitionAsset) return;
		const auto StartupBytes = ReadFile(Root / "Assets/Startup.scene");
		const auto SecondBytes = ReadFile(Root / "Assets/Second.scene");
		const auto RetryBytes = ReadFile(Root / "Assets/Retry.scene");
		CreatedProject.reset();

		SceneRuntimeDesc RuntimeDescription;
		RuntimeDescription.Audio.OutputBackend = AudioOutputBackend::Null;
		RuntimeDescription.Physics.Gravity = glm::vec3(0.0f, -9.81f, 0.0f);
		int PauseAttempts = 0;
		ScriptObservationBuffer ScriptObservations;
		SceneRuntimeServices RuntimeServices;
		RuntimeServices.AudioPauseGuard = [&PauseAttempts](bool Paused) -> std::optional<std::string>
		{
			if (Paused && ++PauseAttempts == 2) return "deterministic candidate activation failure";
			return std::nullopt;
		};
		RuntimeServices.ScriptObserver = { .Context = &ScriptObservations, .Observe = StoreScriptObservation };
		PlayerSession Session(ProjectFile, RuntimeDescription, RuntimeServices);
		PF_CHECK(Tests, !Session.IsRunning());
		const auto TransitionBeforeStart = Session.RequestSceneTransition(SecondAsset->ID);
		PF_CHECK(Tests, !TransitionBeforeStart && TransitionBeforeStart.error().Code == PlayerSessionErrorCode::NotStarted);
		PF_CHECK(Tests, Session.Start().has_value());
		PF_CHECK(Tests, Session.IsRunning());
		PF_CHECK(Tests, Session.GetTimeScale() == 1.0);
		const auto ZeroTimeScale = Session.SetTimeScale(0.0);
		PF_CHECK(Tests, !ZeroTimeScale && ZeroTimeScale.error().Code == PlayerSessionErrorCode::InvalidTimeScale);
		PF_CHECK(Tests, Session.SetTimeScale(0.5).has_value());
		PF_CHECK(Tests, Session.GetTimeScale() == 0.5);
		PF_CHECK(Tests, Session.Advance(Timestep(0.2)).has_value());
		const auto ActiveStartupEntity = Session.GetActiveScene()->FindEntity(StartupEntity->GetUUID());
		PF_CHECK(Tests, ActiveStartupEntity.has_value());
		if (!ActiveStartupEntity) return;
		const auto ScaledTransform = ActiveStartupEntity->GetTransform();
		PF_CHECK(Tests, ScaledTransform && std::abs(ScaledTransform->Translation.x - 0.1f) < 0.0001f);
		PF_CHECK(Tests, Session.SetTimeScale(1.0).has_value());
		PF_CHECK(Tests, Session.GetActiveSceneAsset() == StartupAsset->ID);
		PF_CHECK(Tests, Session.GetActiveScene() && Session.GetActiveScene()->GetEntityCount() == 1);
		const auto DuplicateStart = Session.Start();
		PF_CHECK(Tests, !DuplicateStart && DuplicateStart.error().Code == PlayerSessionErrorCode::AlreadyStarted);

		const Scene* OriginalScene = Session.GetActiveScene();
		const auto NilTransition = Session.RequestSceneTransition(UUID{ 0, 0 });
		PF_CHECK(Tests, !NilTransition && NilTransition.error().Code == PlayerSessionErrorCode::InvalidSceneAsset);
		const auto MissingTransition = Session.RequestSceneTransition(UUID{ 44, 55 });
		PF_CHECK(Tests, !MissingTransition && MissingTransition.error().Code == PlayerSessionErrorCode::SceneLoadFailed);
		PF_CHECK(Tests, Session.GetActiveScene() == OriginalScene);
		PF_CHECK(Tests, Session.GetActiveSceneAsset() == StartupAsset->ID);

		PF_CHECK(Tests, Session.RequestSceneTransition(SecondAsset->ID).has_value());
		PF_CHECK(Tests, Session.GetActiveScene() == OriginalScene);
		PF_CHECK(Tests, Session.Pause().has_value());
		PF_CHECK(Tests, Session.IsPaused());
		const auto FailedCandidateAdvance = Session.Advance(Timestep(0.1));
		PF_CHECK(Tests, !FailedCandidateAdvance && FailedCandidateAdvance.error().Code == PlayerSessionErrorCode::RuntimeOperationFailed);
		PF_CHECK(Tests, !FailedCandidateAdvance &&
			FailedCandidateAdvance.error().Message.find("deterministic candidate activation failure") != std::string::npos);
		const auto RejectedEntity = SecondEntity->GetUUID();
		const auto RejectedAttached = CountScriptObservations(ScriptObservations, ScriptRuntimeObservationKind::ScriptAttached,
			TeardownScriptAsset->ID, RejectedEntity);
		const auto RejectedCreateEntered = CountScriptObservations(ScriptObservations, ScriptRuntimeObservationKind::CallbackEntered,
			TeardownScriptAsset->ID, RejectedEntity, ScriptRuntimeCallback::OnCreate);
		const auto RejectedCreateCompleted = CountScriptObservations(ScriptObservations, ScriptRuntimeObservationKind::CallbackCompleted,
			TeardownScriptAsset->ID, RejectedEntity, ScriptRuntimeCallback::OnCreate);
		const auto RejectedDestroyEntered = CountScriptObservations(ScriptObservations, ScriptRuntimeObservationKind::CallbackEntered,
			TeardownScriptAsset->ID, RejectedEntity, ScriptRuntimeCallback::OnDestroy);
		const auto RejectedDestroyCompleted = CountScriptObservations(ScriptObservations, ScriptRuntimeObservationKind::CallbackCompleted,
			TeardownScriptAsset->ID, RejectedEntity, ScriptRuntimeCallback::OnDestroy);
		const auto RejectedDiagnostics = CountScriptObservations(
			ScriptObservations, ScriptRuntimeObservationKind::Diagnostic, TeardownScriptAsset->ID, RejectedEntity);
		PF_CHECK(Tests, RejectedAttached == 1);
		PF_CHECK(Tests, RejectedCreateEntered == 1);
		PF_CHECK(Tests, RejectedCreateCompleted == 1);
		PF_CHECK(Tests, RejectedDestroyEntered == 1);
		PF_CHECK(Tests, RejectedDestroyCompleted == 1);
		PF_CHECK(Tests, RejectedDiagnostics == 0);
		PF_CHECK(Tests, Session.GetActiveScene() == OriginalScene);
		PF_CHECK(Tests, Session.GetActiveSceneAsset() == StartupAsset->ID);
		PF_CHECK(Tests, Session.IsPaused());
		PF_CHECK(Tests, Session.Resume().has_value());
		PF_CHECK(Tests, Session.Advance(Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, Session.Pause().has_value());
		PF_CHECK(Tests, Session.RequestSceneTransition(RetryAsset->ID).has_value());
		const auto PausedAdvance = Session.Advance(Timestep(0.1));
		PF_CHECK(Tests, PausedAdvance && PausedAdvance->PhysicsSteps == 0);
		PF_CHECK(Tests, Session.GetActiveSceneAsset() == RetryAsset->ID);
		PF_CHECK(Tests, Session.GetActiveScene() != OriginalScene);
		PF_CHECK(Tests, Session.GetActiveScene() && Session.GetActiveScene()->GetEntityCount() == 3);
		const auto RetryEntityID = RetryEntity->GetUUID();
		PF_CHECK(Tests, CountScriptObservations(ScriptObservations, ScriptRuntimeObservationKind::ScriptAttached,
			RetryScriptAsset->ID, RetryEntityID) == 1);
		PF_CHECK(Tests, CountScriptObservations(ScriptObservations, ScriptRuntimeObservationKind::CallbackCompleted,
			RetryScriptAsset->ID, RetryEntityID, ScriptRuntimeCallback::OnCreate) == 1);
		PF_CHECK(Tests, CountScriptObservations(ScriptObservations, ScriptRuntimeObservationKind::CallbackEntered,
			ScriptAsset->ID, StartupEntity->GetUUID(), ScriptRuntimeCallback::OnDestroy) == 1);
		PF_CHECK(Tests, CountScriptObservations(ScriptObservations, ScriptRuntimeObservationKind::CallbackCompleted,
			ScriptAsset->ID, StartupEntity->GetUUID(), ScriptRuntimeCallback::OnDestroy) == 1);
		PF_CHECK(Tests, CountScriptObservations(ScriptObservations, ScriptRuntimeObservationKind::Diagnostic,
			ScriptAsset->ID, StartupEntity->GetUUID()) == 0);
		PF_CHECK(Tests, Session.Resume().has_value());
		PF_CHECK(Tests, !Session.IsPaused());
		PF_CHECK(Tests, Session.Advance(Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, Session.GetActiveSceneAsset() == RetryAsset->ID);
		PF_CHECK(Tests, Session.Advance(Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, Session.GetActiveSceneAsset() == RetryAsset->ID);
		PF_CHECK(Tests, CountScriptObservations(ScriptObservations, ScriptRuntimeObservationKind::Diagnostic,
			CandidateRequestScriptAsset->ID, CandidateRequestEntity->GetUUID()) > 0);

		PF_CHECK(Tests, !StartupBytes.empty());
		PF_CHECK(Tests, Session.GetActiveScene()->CreateEntity("Runtime Only Mutation").has_value());
		Session.Stop();
		PF_CHECK(Tests, !Session.IsRunning());
		PF_CHECK(Tests, !Session.GetActiveScene());
		PF_CHECK(Tests, !Session.GetActiveSceneAsset());
		PF_CHECK(Tests, ReadFile(Root / "Assets/Startup.scene") == StartupBytes);
		PF_CHECK(Tests, ReadFile(Root / "Assets/Second.scene") == SecondBytes);
		PF_CHECK(Tests, ReadFile(Root / "Assets/Retry.scene") == RetryBytes);
		PF_CHECK(Tests, CountScriptObservations(ScriptObservations, ScriptRuntimeObservationKind::CallbackEntered,
			RetryScriptAsset->ID, RetryEntityID, ScriptRuntimeCallback::OnDestroy) == 1);
		PF_CHECK(Tests, CountScriptObservations(ScriptObservations, ScriptRuntimeObservationKind::CallbackCompleted,
			RetryScriptAsset->ID, RetryEntityID, ScriptRuntimeCallback::OnDestroy) == 1);
		PF_CHECK(Tests, CountScriptObservations(ScriptObservations, ScriptRuntimeObservationKind::Diagnostic,
			RetryScriptAsset->ID, RetryEntityID) == 0);
		PF_CHECK(Tests, CountScriptObservations(ScriptObservations, ScriptRuntimeObservationKind::CallbackEntered,
			TeardownScriptAsset->ID, RejectedEntity, ScriptRuntimeCallback::OnDestroy) == 1);
		PF_CHECK(Tests, CountScriptObservations(ScriptObservations, ScriptRuntimeObservationKind::CallbackCompleted,
			TeardownScriptAsset->ID, RejectedEntity, ScriptRuntimeCallback::OnDestroy) == 1);
		PF_CHECK(Tests, CountScriptObservations(ScriptObservations, ScriptRuntimeObservationKind::Diagnostic,
			TeardownScriptAsset->ID, RejectedEntity) == 0);
		PF_CHECK(Tests, CountScriptObservations(ScriptObservations, ScriptRuntimeObservationKind::CallbackEntered,
			ScriptAsset->ID, StartupEntity->GetUUID(), ScriptRuntimeCallback::OnDestroy) == 1);
		PF_CHECK(Tests, CountScriptObservations(ScriptObservations, ScriptRuntimeObservationKind::CallbackCompleted,
			ScriptAsset->ID, StartupEntity->GetUUID(), ScriptRuntimeCallback::OnDestroy) == 1);
		PF_CHECK(Tests, !ScriptObservations.Overflowed);
		const auto AdvanceAfterStop = Session.Advance(Timestep(0.01));
		PF_CHECK(Tests, !AdvanceAfterStop && AdvanceAfterStop.error().Code == PlayerSessionErrorCode::NotStarted);

		PlayerSession RejectedTransitionSession(ProjectFile, RuntimeDescription, RuntimeServices);
		PF_CHECK(Tests, RejectedTransitionSession.Start(DeferredTransitionAsset->ID).has_value());
		const Scene* DeferredSceneBeforeFailure = RejectedTransitionSession.GetActiveScene();
		const auto DeferredEntityBefore = DeferredSceneBeforeFailure->FindEntity(DeferredTransitionEntity->GetUUID());
		const auto DeferredPhysicsBefore = DeferredSceneBeforeFailure->FindEntity(DeferredPhysicsEntity->GetUUID());
		PF_CHECK(Tests, DeferredEntityBefore.has_value() && DeferredPhysicsBefore.has_value());
		if (!DeferredEntityBefore || !DeferredPhysicsBefore) return;
		const auto DeferredTransformBefore = DeferredEntityBefore->GetTransform();
		const auto DeferredPhysicsTransformBefore = DeferredPhysicsBefore->GetTransform();
		PF_CHECK(Tests, DeferredTransformBefore.has_value() && DeferredPhysicsTransformBefore.has_value());
		const auto RejectedDeferredLoad = RejectedTransitionSession.Advance(Timestep(0.1));
		PF_CHECK(Tests, !RejectedDeferredLoad && RejectedDeferredLoad.error().Code == PlayerSessionErrorCode::SceneLoadFailed);
		PF_CHECK(Tests, RejectedTransitionSession.GetActiveScene() == DeferredSceneBeforeFailure);
		PF_CHECK(Tests, RejectedTransitionSession.GetActiveSceneAsset() == DeferredTransitionAsset->ID);
		const auto QueuedRuntimeFailure = RejectedTransitionSession.Advance(Timestep(0.1));
		PF_CHECK(Tests, QueuedRuntimeFailure.has_value());
		if (!QueuedRuntimeFailure) return;
		PF_CHECK(Tests, RejectedTransitionSession.GetActiveScene() == DeferredSceneBeforeFailure);
		const auto CandidateRuntimeFailure = RejectedTransitionSession.Advance(Timestep(0.1));
		PF_CHECK(Tests, !CandidateRuntimeFailure &&
			CandidateRuntimeFailure.error().Code == PlayerSessionErrorCode::RuntimeStartFailed);
		PF_CHECK(Tests, RejectedTransitionSession.GetActiveScene() == DeferredSceneBeforeFailure);
		PF_CHECK(Tests, RejectedTransitionSession.GetActiveSceneAsset() == DeferredTransitionAsset->ID);
		const auto ResumedDeferredFrame = RejectedTransitionSession.Advance(Timestep(0.1));
		PF_CHECK(Tests, ResumedDeferredFrame.has_value());
		if (!ResumedDeferredFrame) return;
		const auto DeferredEntityAfter = RejectedTransitionSession.GetActiveScene()->FindEntity(DeferredTransitionEntity->GetUUID());
		const auto DeferredPhysicsAfter = RejectedTransitionSession.GetActiveScene()->FindEntity(DeferredPhysicsEntity->GetUUID());
		PF_CHECK(Tests, DeferredEntityAfter.has_value() && DeferredPhysicsAfter.has_value());
		if (!DeferredEntityAfter || !DeferredPhysicsAfter) return;
		const auto DeferredTransformAfter = DeferredEntityAfter->GetTransform();
		const auto DeferredPhysicsTransformAfter = DeferredPhysicsAfter->GetTransform();
		PF_CHECK(Tests, DeferredTransformAfter.has_value() && DeferredPhysicsTransformAfter.has_value());
		if (DeferredTransformBefore && DeferredTransformAfter)
			PF_CHECK(Tests, DeferredTransformAfter->Translation.x > DeferredTransformBefore->Translation.x);
		if (DeferredPhysicsTransformBefore && DeferredPhysicsTransformAfter)
			PF_CHECK(Tests, DeferredPhysicsTransformAfter->Translation.y < DeferredPhysicsTransformBefore->Translation.y);
		PF_CHECK(Tests, RejectedTransitionSession.RequestSceneTransition(RetryAsset->ID).has_value());
		PF_CHECK(Tests, RejectedTransitionSession.Advance(Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, RejectedTransitionSession.GetActiveSceneAsset() == RetryAsset->ID);
		PF_CHECK(Tests, RejectedTransitionSession.Advance(Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, RejectedTransitionSession.GetActiveSceneAsset() == RetryAsset->ID);
		PF_CHECK(Tests, RejectedTransitionSession.IsRunning());
		PF_CHECK(Tests, CountScriptObservations(ScriptObservations, ScriptRuntimeObservationKind::Diagnostic,
			TeardownRequestScriptAsset->ID, TeardownRequestEntity->GetUUID()) > 0);
		RejectedTransitionSession.Stop();

		const std::array Override = { SecondAsset->ID };
		PlayerSession OverrideSession(ProjectFile, RuntimeDescription);
		PF_CHECK(Tests, OverrideSession.Start(Override.front()).has_value());
		PF_CHECK(Tests, OverrideSession.GetActiveSceneAsset() == SecondAsset->ID);
		OverrideSession.Stop();

		std::filesystem::create_directories(Root / "Other Project", Error);
		PF_CHECK(Tests, !Error);
		auto SecondProjectResult = Project::Create(Root / "Other Project" / "Other.pfproj", "Other Project");
		PF_CHECK(Tests, SecondProjectResult.has_value());
		if (!SecondProjectResult) return;
		std::optional<Project> SecondProject(std::move(*SecondProjectResult));
		Scene OtherProjectScene;
		PF_CHECK(Tests, OtherProjectScene.CreateEntity("Other Project Entity").has_value());
		std::filesystem::create_directories(SecondProject->GetRootPath() / "Assets" / "Levels", Error);
		PF_CHECK(Tests, !Error);
		auto OtherProjectAsset = SceneAssetService::Create(
			SecondProject->GetAssetRegistry(), SecondProject->GetRootPath(), "Assets/Levels/Other.scene", OtherProjectScene);
		PF_CHECK(Tests, OtherProjectAsset.has_value());
		if (!OtherProjectAsset) return;
		PF_CHECK(Tests, SecondProject->SetStartScene(OtherProjectAsset->ID).has_value());
		const auto SecondProjectID = SecondProject->GetDescription().ID;
		SecondProject.reset();
		PlayerSession OtherProjectSession(Root / "Other Project" / "Other.pfproj", RuntimeDescription);
		PF_CHECK(Tests, OtherProjectSession.Start().has_value());
		PF_CHECK(Tests, OtherProjectSession.GetProject() && OtherProjectSession.GetProject()->GetDescription().ID == SecondProjectID);
		PF_CHECK(Tests, OtherProjectSession.GetActiveSceneAsset() == OtherProjectAsset->ID);
		OtherProjectSession.Stop();

		std::filesystem::create_directories(Root / "Empty Project", Error);
		PF_CHECK(Tests, !Error);
		auto EmptyProjectResult = Project::Create(Root / "Empty Project" / "Empty.pfproj", "Empty Project");
		PF_CHECK(Tests, EmptyProjectResult.has_value());
		if (!EmptyProjectResult) return;
		std::optional<Project> EmptyProject(std::move(*EmptyProjectResult));
		EmptyProject.reset();
		PlayerSession MissingStartupSession(Root / "Empty Project" / "Empty.pfproj", RuntimeDescription);
		const auto MissingStartup = MissingStartupSession.Start();
		PF_CHECK(Tests, !MissingStartup && MissingStartup.error().Code == PlayerSessionErrorCode::MissingStartupScene);
	}

	void TestSourcePlayerRejectsStaleBlueprintGraphPair(TestRunner& Tests)
	{
		using namespace PulseForge;
		const auto Identifier = UUID::Generate();
		PF_CHECK(Tests, Identifier.has_value());
		if (!Identifier) return;
		std::error_code Error;
		const auto Root = std::filesystem::temp_directory_path(Error) /
			("PulseForge Stale Blueprint Player " + Identifier->ToString());
		PF_CHECK(Tests, !Error);
		if (Error) return;
		struct DirectoryCleanup
		{
			std::filesystem::path Path;
			~DirectoryCleanup() { std::error_code CleanupError; std::filesystem::remove_all(Path, CleanupError); }
		} Cleanup{ Root };

		auto ProjectResult = Project::Create(Root / "Source.pfproj", "Stale Blueprint Player");
		PF_CHECK(Tests, ProjectResult.has_value());
		if (!ProjectResult) return;
		std::optional<Project> ProjectValue(std::move(*ProjectResult));
		const std::string SavedGraph = "graph source revision 1";
		const std::string InterruptedGraph = "graph source revision 0";
		BlueprintProgramSource ProgramSource;
		ProgramSource.Constants = { 1.0f };
		ProgramSource.ScalarPropertyIds = { "score" };
		ProgramSource.Instructions = {
			{ BlueprintOpcode::LoadScalar, 0, 0, UUID(0x91, 1) },
			{ BlueprintOpcode::PushConstant, 0, 0, UUID(0x91, 2) },
			{ BlueprintOpcode::Add, 0, 0, UUID(0x91, 3) },
			{ BlueprintOpcode::StoreScalar, 0, 0, UUID(0x91, 4) },
			{ BlueprintOpcode::LoadScalar, 0, 0, UUID(0x91, 5) },
			{ BlueprintOpcode::WriteScalarProperty, 0, 0, UUID(0x91, 6) },
			{ BlueprintOpcode::Return, 0, 0, UUID(0x91, 7) }
		};
		const std::array Events{ BlueprintEventProgramSource{ BlueprintEventKind::OnUpdate, ProgramSource } };
		const auto ProgramBytes = SerializeBlueprintAsset(Events,
			ComputeBlueprintSourceRevision(std::as_bytes(std::span<const char>(SavedGraph.data(), SavedGraph.size()))));
		PF_CHECK(Tests, ProgramBytes.has_value());
		if (!ProgramBytes) return;
		const auto BehaviorAsset = AssetOperations::CreateAssetFromBytes(ProjectValue->GetAssetRegistry(),
			ProjectValue->GetRootPath(), std::span<const std::byte>(*ProgramBytes), "Assets/StaleBehavior.pfblueprint");
		PF_CHECK(Tests, BehaviorAsset.has_value());
		if (!BehaviorAsset) return;
		const auto GraphPath = ProjectValue->GetRootPath() / ".pulseforge" / "blueprint-graphs" /
			(BehaviorAsset->ID.ToString() + ".pfg");
		std::filesystem::create_directories(GraphPath.parent_path(), Error);
		PF_CHECK(Tests, !Error);
		if (Error) return;
		auto WriteText = [](const std::filesystem::path& Path, std::string_view Text)
		{
			std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
			Output.write(Text.data(), static_cast<std::streamsize>(Text.size()));
			return static_cast<bool>(Output);
		};
		PF_CHECK(Tests, WriteText(GraphPath, InterruptedGraph));
		Scene Startup;
		auto EntityValue = Startup.CreateEntity("Stale graph source behavior");
		PF_CHECK(Tests, EntityValue.has_value());
		if (!EntityValue) return;
		BlueprintComponent Blueprint{ BehaviorAsset->ID, true };
		PF_CHECK(Tests, Blueprint.SetFloatProperty("score", 0.0f).has_value());
		PF_CHECK(Tests, EntityValue->SetBlueprint(Blueprint).has_value());
		const UUID EntityID = EntityValue->GetUUID();
		auto SceneAsset = SceneAssetService::Create(ProjectValue->GetAssetRegistry(), ProjectValue->GetRootPath(),
			"Assets/Startup.scene", Startup);
		PF_CHECK(Tests, SceneAsset.has_value());
		if (!SceneAsset) return;
		PF_CHECK(Tests, ProjectValue->SetStartScene(SceneAsset->ID).has_value());

		SceneRuntimeDesc RuntimeDescription;
		RuntimeDescription.Audio.OutputBackend = AudioOutputBackend::Null;
		PlayerSession Session(ProjectValue->GetFilePath(), RuntimeDescription);
		PF_CHECK(Tests, Session.Start().has_value());
		PF_CHECK(Tests, Session.Advance(Timestep(1.0 / 60.0)).has_value());
		auto StaleEntity = Session.GetActiveScene()->FindEntity(EntityID);
		PF_CHECK(Tests, StaleEntity.has_value());
		if (StaleEntity)
		{
			const auto Value = StaleEntity->GetBlueprint();
			PF_CHECK(Tests, Value && Value->has_value() && (**Value).FindFloatProperty("score") == 0.0f);
		}
		Session.Stop();
		PF_CHECK(Tests, WriteText(GraphPath, SavedGraph));
		PlayerSession RepairedSession(ProjectValue->GetFilePath(), RuntimeDescription);
		PF_CHECK(Tests, RepairedSession.Start().has_value());
		PF_CHECK(Tests, RepairedSession.Advance(Timestep(1.0 / 60.0)).has_value());
		const auto CurrentEntity = RepairedSession.GetActiveScene()->FindEntity(EntityID);
		PF_CHECK(Tests, CurrentEntity.has_value());
		if (CurrentEntity)
		{
			const auto Value = CurrentEntity->GetBlueprint();
			PF_CHECK(Tests, Value && Value->has_value() && (**Value).FindFloatProperty("score") == 1.0f);
		}
		RepairedSession.Stop();

		auto LegacyEnvelope = SerializeBlueprintAsset(Events);
		PF_CHECK(Tests, LegacyEnvelope.has_value());
		if (!LegacyEnvelope) return;
		std::vector<std::byte> LegacyProgramBytes(LegacyEnvelope->begin(), LegacyEnvelope->begin() + 9);
		LegacyProgramBytes.insert(LegacyProgramBytes.end(), LegacyEnvelope->begin() + 17, LegacyEnvelope->end());
		LegacyProgramBytes[4] = std::byte{ static_cast<uint8_t>(BlueprintLegacyUnboundAssetFormatVersion) };
		const auto LegacyProgram = DeserializeBlueprintAsset(LegacyProgramBytes);
		PF_CHECK(Tests, LegacyProgram && LegacyProgram->GetVersion() == BlueprintLegacyUnboundAssetFormatVersion &&
			LegacyProgram->GetSourceRevision() == 0);
		if (!LegacyProgram) return;
		const auto LegacyBehaviorAsset = AssetOperations::CreateAssetFromBytes(ProjectValue->GetAssetRegistry(),
			ProjectValue->GetRootPath(), std::span<const std::byte>(LegacyProgramBytes), "Assets/LegacyBehavior.pfblueprint");
		PF_CHECK(Tests, LegacyBehaviorAsset.has_value());
		if (!LegacyBehaviorAsset) return;
		const auto LegacyGraphPath = ProjectValue->GetRootPath() / ".pulseforge" / "blueprint-graphs" /
			(LegacyBehaviorAsset->ID.ToString() + ".pfg");
		std::error_code LegacyGraphError;
		std::filesystem::remove(LegacyGraphPath, LegacyGraphError);
		PF_CHECK(Tests, !LegacyGraphError && !std::filesystem::exists(LegacyGraphPath));
		Scene LegacyStartup;
		auto LegacyEntity = LegacyStartup.CreateEntity("Legacy source Blueprint");
		PF_CHECK(Tests, LegacyEntity.has_value());
		if (!LegacyEntity) return;
		BlueprintComponent LegacyComponent{ LegacyBehaviorAsset->ID, true };
		PF_CHECK(Tests, LegacyComponent.SetFloatProperty("score", 0.0f).has_value());
		PF_CHECK(Tests, LegacyEntity->SetBlueprint(LegacyComponent).has_value());
		const UUID LegacyEntityID = LegacyEntity->GetUUID();
		auto LegacySceneAsset = SceneAssetService::Create(ProjectValue->GetAssetRegistry(), ProjectValue->GetRootPath(),
			"Assets/LegacyStartup.scene", LegacyStartup);
		PF_CHECK(Tests, LegacySceneAsset.has_value());
		if (!LegacySceneAsset) return;
		PF_CHECK(Tests, ProjectValue->SetStartScene(LegacySceneAsset->ID).has_value());
		PlayerSession LegacySession(ProjectValue->GetFilePath(), RuntimeDescription);
		PF_CHECK(Tests, LegacySession.Start().has_value());
		PF_CHECK(Tests, LegacySession.Advance(Timestep(1.0 / 60.0)).has_value());
		const auto LoadedLegacyEntity = LegacySession.GetActiveScene()->FindEntity(LegacyEntityID);
		PF_CHECK(Tests, LoadedLegacyEntity.has_value());
		if (LoadedLegacyEntity)
		{
			const auto Value = LoadedLegacyEntity->GetBlueprint();
			PF_CHECK(Tests, Value && Value->has_value() && (**Value).FindFloatProperty("score") == 1.0f);
		}
		LegacySession.Stop();
	}

	void TestCookedPlayerSessionDoesNotOpenSourceProject(TestRunner& Tests)
	{
		using namespace PulseForge;
		const auto ProjectIdentifier = UUID::Generate();
		const auto SceneIdentifier = UUID::Generate();
		PF_CHECK(Tests, ProjectIdentifier.has_value() && SceneIdentifier.has_value());
		if (!ProjectIdentifier || !SceneIdentifier) return;

		std::error_code Error;
		const auto Temporary = std::filesystem::canonical(std::filesystem::temp_directory_path(Error), Error);
		PF_CHECK(Tests, !Error);
		if (Error) return;
		const auto Root = Temporary / ("PulseForge Cooked Player Session " + ProjectIdentifier->ToString());
		std::filesystem::create_directories(Root / "products", Error);
		PF_CHECK(Tests, !Error);
		if (Error) return;
		struct DirectoryCleanup
		{
			std::filesystem::path Path;
			~DirectoryCleanup() { std::error_code CleanupError; std::filesystem::remove_all(Path, CleanupError); }
		} Cleanup{ Root };

		Scene AuthoredScene;
		const auto ScriptIdentifier = UUID::Generate();
		PF_CHECK(Tests, ScriptIdentifier.has_value());
		if (!ScriptIdentifier) return;
		const auto BlueprintIdentifier = UUID::Generate();
		PF_CHECK(Tests, BlueprintIdentifier.has_value());
		if (!BlueprintIdentifier) return;
		auto AuthoredEntity = AuthoredScene.CreateEntity("Cooked Runtime Entity");
		PF_CHECK(Tests, AuthoredEntity.has_value());
		if (!AuthoredEntity) return;
		const AssetID SkeletalAssetIdentifier = UUID::Generate().value_or(UUID{});
		PF_CHECK(Tests, !SkeletalAssetIdentifier.IsNil());
		if (SkeletalAssetIdentifier.IsNil()) return;
		constexpr std::string_view SkeletalProductKey = "skeletal/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
		constexpr std::string_view AnimationClipKey = "animation/bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
		MeshRendererComponent SkeletalComponent;
		SkeletalComponent.MeshAsset = SkeletalAssetIdentifier;
		SkeletalComponent.SkeletalProductKey = std::string(SkeletalProductKey);
		SkeletalComponent.AnimationClipKey = std::string(AnimationClipKey);
		SkeletalComponent.AnimationLooping = false;
		SkeletalComponent.AnimationSpeed = 2.0f;
		PF_CHECK(Tests, AuthoredEntity->SetMeshRenderer(SkeletalComponent).has_value());
		auto SecondSkeletalEntity = AuthoredScene.CreateEntity("Second cooked animation instance");
		PF_CHECK(Tests, SecondSkeletalEntity.has_value());
		if (!SecondSkeletalEntity) return;
		PF_CHECK(Tests, SecondSkeletalEntity->SetMeshRenderer(SkeletalComponent).has_value());

		ImportedGltfSkeletalMesh SkeletalProduct;
		SkeletalProduct.Geometry.Vertices.resize(3);
		SkeletalProduct.Geometry.Vertices[0].Position[0] = 0.0f;
		SkeletalProduct.Geometry.Vertices[1].Position[0] = 1.0f;
		SkeletalProduct.Geometry.Vertices[2].Position[1] = 1.0f;
		SkeletalProduct.Geometry.Indices = { 0, 1, 2 };
		ImportedGltfSkeletonNode MeshNode;
		MeshNode.StableKey = "node/mesh";
		MeshNode.Name = "mesh";
		MeshNode.LocalBindMatrix = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
		ImportedGltfSkeletonNode JointNode;
		JointNode.StableKey = "node/mesh/joint";
		JointNode.Name = "joint";
		JointNode.ParentNodeIndex = 0;
		JointNode.IsJoint = true;
		JointNode.LocalBindMatrix = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
		SkeletalProduct.SkeletonNodes = { MeshNode, JointNode };
		SkeletalProduct.JointNodeIndices = { 1 };
		SkeletalProduct.InverseBindMatrices = { { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 } };
		SkeletalProduct.VertexInfluences.resize(3);
		for (GltfVertexInfluences& Influence : SkeletalProduct.VertexInfluences)
		{
			Influence.JointIndices = { 0, 0, 0, 0 };
			Influence.Weights = { 1, 0, 0, 0 };
		}
		ImportedGltfAnimationClip RuntimeClip;
		RuntimeClip.StableKey = std::string(AnimationClipKey);
		RuntimeClip.Name = "move-up";
		RuntimeClip.Duration = 1.0f;
		ImportedGltfAnimationChannel RuntimeChannel;
		RuntimeChannel.TargetNodeKey = JointNode.StableKey;
		RuntimeChannel.Path = GltfAnimationPath::Translation;
		RuntimeChannel.Times = { 0.0f, 1.0f };
		RuntimeChannel.Values = { { 0,0,0,0 }, { 0,1,0,0 } };
		RuntimeClip.Channels.push_back(std::move(RuntimeChannel));
		RuntimeClip.EventMarkers = {
			{ "event/first-clip-end", "FirstClipEnd", 0.9f },
			{ "event/first-clip-loop", "FirstClipLoop", 0.0f } };
		SkeletalProduct.Animations.push_back(std::move(RuntimeClip));
		ImportedGltfAnimationClip SecondRuntimeClip;
		SecondRuntimeClip.StableKey = "animation/cccccccccccccccccccccccccccccccc";
		SecondRuntimeClip.Name = "move-down";
		SecondRuntimeClip.Duration = 2.0f;
		ImportedGltfAnimationChannel SecondRuntimeChannel;
		SecondRuntimeChannel.TargetNodeKey = JointNode.StableKey;
		SecondRuntimeChannel.Path = GltfAnimationPath::Translation;
		SecondRuntimeChannel.Times = { 0.0f, 2.0f };
		SecondRuntimeChannel.Values = { { 0,0,0,0 }, { 0,-1,0,0 } };
		SecondRuntimeClip.Channels.push_back(std::move(SecondRuntimeChannel));
		SecondRuntimeClip.EventMarkers.push_back({ "event/second-clip-half", "SecondClipHalf", 0.5f });
		SkeletalProduct.Animations.push_back(std::move(SecondRuntimeClip));
		auto SkeletalProductBytes = Detail::EncodeSkeletalMeshProduct(SkeletalProduct);
		PF_CHECK(Tests, SkeletalProductBytes.has_value());
		if (!SkeletalProductBytes) return;
		PF_CHECK(Tests, AuthoredEntity->SetScript(ScriptComponent{ *ScriptIdentifier }).has_value());
		BlueprintComponent BlueprintDefaults{ *BlueprintIdentifier, true, {} };
		PF_CHECK(Tests, BlueprintDefaults.SetFloatProperty("runtime_value", 10.0f).has_value());
		PF_CHECK(Tests, AuthoredEntity->SetBlueprint(BlueprintDefaults).has_value());
		PF_CHECK(Tests, AuthoredEntity->SetRigidbody({ RigidbodyMotionType::Dynamic, 1.0f, 0.2f, 0.0f, false }).has_value());
		PF_CHECK(Tests, AuthoredEntity->SetBoxCollider({ { 0.5f, 0.5f, 0.5f } }).has_value());
		const auto AudioIdentifier = UUID::Generate();
		PF_CHECK(Tests, AudioIdentifier.has_value());
		if (!AudioIdentifier) return;
		PF_CHECK(Tests, AuthoredEntity->SetAudioSource(AudioSourceComponent{ *AudioIdentifier, 1.0f, true, true, false }).has_value());
		auto Serialized = SceneSerializer::Serialize(AuthoredScene);
		PF_CHECK(Tests, Serialized.has_value());
		if (!Serialized) return;
		const std::span<const std::byte> PayloadBytes = std::as_bytes(std::span(Serialized->data(), Serialized->size()));
		const std::vector<std::byte> Payload(PayloadBytes.begin(), PayloadBytes.end());
		{
			std::ofstream Output(Root / "products" / "startup.scene", std::ios::binary | std::ios::trunc);
			Output.write(reinterpret_cast<const char*>(Payload.data()), static_cast<std::streamsize>(Payload.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		{
			std::ofstream Output(Root / "products" / "character.skeletal.bin", std::ios::binary | std::ios::trunc);
			Output.write(reinterpret_cast<const char*>(SkeletalProductBytes->data()),
				static_cast<std::streamsize>(SkeletalProductBytes->size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const CookedAssetDigest Digest = CookedAssetCatalog::ComputePayloadDigest(Payload);
		CookedAssetEntry Entry{ *SceneIdentifier, CookedAssetProductKind::Scene, {}, "products/startup.scene", 1,
			Payload.size(), Digest.Algorithm, Digest.Value, {} };
		const CookedAssetDigest SkeletalDigest = CookedAssetCatalog::ComputePayloadDigest(*SkeletalProductBytes);
		CookedAssetEntry SkeletalEntry{ SkeletalAssetIdentifier, CookedAssetProductKind::SkeletalMesh,
			"skeletal/runtime-fixture", "products/character.skeletal.bin", Detail::SkeletalMeshProductSchemaVersion,
			SkeletalProductBytes->size(), SkeletalDigest.Algorithm, SkeletalDigest.Value, {} };
		SkeletalEntry.StableProductKey = std::string(SkeletalProductKey);
		const std::string Script =
			"function OnCreate() assert(entity:play_audio()) end\n"
			"function OnUpdate(dt) assert(entity:set_translation(dt, 0, 0)) end\n";
		const std::span<const std::byte> ScriptSpan = std::as_bytes(std::span(Script.data(), Script.size()));
		const std::vector<std::byte> ScriptBytes(ScriptSpan.begin(), ScriptSpan.end());
		const CookedAssetDigest ScriptDigest = CookedAssetCatalog::ComputePayloadDigest(ScriptBytes);
		{
			std::ofstream Output(Root / "products" / "script.lua", std::ios::binary | std::ios::trunc);
			Output.write(Script.data(), static_cast<std::streamsize>(Script.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		CookedAssetEntry ScriptEntry{ *ScriptIdentifier, CookedAssetProductKind::Script, {}, "products/script.lua", 1,
			ScriptBytes.size(), ScriptDigest.Algorithm, ScriptDigest.Value, {} };
		BlueprintProgramSource BlueprintOnUpdate;
		BlueprintOnUpdate.Constants = { 1.0f };
		BlueprintOnUpdate.ScalarPropertyIds = { "runtime_value" };
		BlueprintOnUpdate.Instructions = {
			{ BlueprintOpcode::LoadScalar, 0, 0, UUID(0x50, 1) },
			{ BlueprintOpcode::PushConstant, 0, 0, UUID(0x50, 2) },
			{ BlueprintOpcode::Add, 0, 0, UUID(0x50, 3) },
			{ BlueprintOpcode::StoreScalar, 0, 0, UUID(0x50, 4) },
			{ BlueprintOpcode::LoadScalar, 0, 0, UUID(0x50, 5) },
			{ BlueprintOpcode::WriteScalarProperty, 0, 0, UUID(0x50, 6) },
			{ BlueprintOpcode::Return, 0, 0, UUID(0x50, 7) }
		};
		const std::array BlueprintEvents{ BlueprintEventProgramSource{ BlueprintEventKind::OnUpdate, BlueprintOnUpdate } };
		auto BlueprintBytes = SerializeBlueprintAsset(BlueprintEvents);
		PF_CHECK(Tests, BlueprintBytes.has_value());
		if (!BlueprintBytes) return;
		{
			std::ofstream Output(Root / "products" / "behavior.pfbp", std::ios::binary | std::ios::trunc);
			Output.write(reinterpret_cast<const char*>(BlueprintBytes->data()), static_cast<std::streamsize>(BlueprintBytes->size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const CookedAssetDigest BlueprintDigest = CookedAssetCatalog::ComputePayloadDigest(*BlueprintBytes);
		CookedAssetEntry BlueprintEntry{ *BlueprintIdentifier, CookedAssetProductKind::BlueprintProgram, {},
			"products/behavior.pfbp", BlueprintAssetFormatVersion, BlueprintBytes->size(),
			BlueprintDigest.Algorithm, BlueprintDigest.Value, {} };
		const std::vector<std::byte> WaveBytes = MakeCookedSilentWave();
		{
			std::ofstream Output(Root / "products" / "sound.wav", std::ios::binary | std::ios::trunc);
			Output.write(reinterpret_cast<const char*>(WaveBytes.data()), static_cast<std::streamsize>(WaveBytes.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const CookedAssetDigest AudioDigest = CookedAssetCatalog::ComputePayloadDigest(WaveBytes);
		CookedAssetEntry AudioEntry{ *AudioIdentifier, CookedAssetProductKind::Audio, {}, "products/sound.wav", 1,
			WaveBytes.size(), AudioDigest.Algorithm, AudioDigest.Value, {} };
		const CookedAssetCompatibility Compatibility{ "test-target", "test-renderer", "test-runtime-abi" };
		auto Manifest = CookedAssetCatalog::SerializeManifest(
			*ProjectIdentifier, *SceneIdentifier, Compatibility, {},
			{ std::move(Entry), std::move(ScriptEntry), std::move(AudioEntry), std::move(BlueprintEntry),
				std::move(SkeletalEntry) });
		PF_CHECK(Tests, Manifest.has_value());
		if (!Manifest) return;
		{
			std::ofstream Output(Root / CookedAssetCatalog::ManifestFileName, std::ios::binary | std::ios::trunc);
			Output.write(Manifest->data(), static_cast<std::streamsize>(Manifest->size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		auto Provider = CookedContentProvider::Open(Root, Compatibility);
		PF_CHECK(Tests, Provider.has_value());
		if (!Provider) return;

		ScriptObservationBuffer ScriptObservations;
		uint64_t BlueprintProgramGeneration = 1;
		SceneRuntimeServices RuntimeServices;
		RuntimeServices.ScriptObserver = { &ScriptObservations, StoreScriptObservation };
		RuntimeServices.GetBlueprintProgramGeneration = [&BlueprintProgramGeneration] { return BlueprintProgramGeneration; };
		SceneRuntimeDesc RuntimeDescription;
		RuntimeDescription.Audio.OutputBackend = AudioOutputBackend::Null;
		PlayerSession Session(*Provider, RuntimeDescription, RuntimeServices);
		PF_CHECK(Tests, Session.Start().has_value());
		PF_CHECK(Tests, Session.IsCookedSession());
		PF_CHECK(Tests, Session.GetProject() == nullptr);
		PF_CHECK(Tests, !std::filesystem::exists(Root / "SourceProject.pfproj"));
		PF_CHECK(Tests, !std::filesystem::exists(Root / "Assets"));
		PF_CHECK(Tests, Session.GetActiveSceneAsset() == *SceneIdentifier);
		PF_CHECK(Tests, Session.GetActiveScene() && Session.GetActiveScene()->GetEntityCount() == 2);
		const auto CookedEntityAtStart = Session.GetActiveScene()->FindEntity(AuthoredEntity->GetUUID());
		PF_CHECK(Tests, CookedEntityAtStart.has_value());
		if (CookedEntityAtStart)
		{
			const auto CookedBlueprint = CookedEntityAtStart->GetBlueprint();
			PF_CHECK(Tests, CookedBlueprint && CookedBlueprint->has_value() &&
				(**CookedBlueprint).FindFloatProperty("runtime_value") == 10.0f);
		}
		auto Advanced = Session.Advance(Timestep(1.0 / 60.0));
		PF_CHECK(Tests, Advanced.has_value());
		PF_CHECK(Tests, Advanced && Advanced->PhysicsSteps > 0);
		const auto FirstRuntimePose = Session.GetActiveScene()->GetRuntimeSkeletalPose(AuthoredEntity->GetUUID());
		PF_CHECK(Tests, FirstRuntimePose.has_value() && FirstRuntimePose->size() == 2);
		if (FirstRuntimePose && FirstRuntimePose->size() == 2)
			PF_CHECK(Tests, std::abs((*FirstRuntimePose)[1][13] - (2.0f / 60.0f)) < 1.0e-4f);
		if (CookedEntityAtStart)
		{
			const auto Playback = Session.GetSkeletalAnimationPlayback(*CookedEntityAtStart);
			PF_CHECK(Tests, Playback && Playback->ProductKey == SkeletalProductKey &&
				Playback->CurrentClipKey == AnimationClipKey && Playback->DurationSeconds == 1.0f &&
				Playback->Clips.size() == 2 && !Playback->Paused && !Playback->Crossfading);
		}
		PF_CHECK(Tests, !AuthoredScene.GetRuntimeSkeletalPose(AuthoredEntity->GetUUID()).has_value());
		auto ReadRuntimeValue = [&Tests, &Session, &AuthoredEntity](float ExpectedValue)
		{
			const auto Traces = Session.GetBlueprintTraces();
			const auto Trace = std::find_if(Traces.begin(), Traces.end(), [&](const BlueprintRuntimeTrace& Candidate)
				{ return Candidate.Handle.Target.GetUUID() == AuthoredEntity->GetUUID(); });
			PF_CHECK(Tests, Trace != Traces.end());
			if (Trace == Traces.end()) return;
			const auto Current = Session.ReadBlueprintTrace(Trace->Handle);
			PF_CHECK(Tests, Current && Current->ScalarPropertyIds == std::vector<std::string>{ "runtime_value" } &&
				Current->ScalarValues == std::vector<float>{ ExpectedValue });
		};
		ReadRuntimeValue(11.0f);
		const auto CookedBlueprintAfterFirstUpdate = Session.GetActiveScene()->FindEntity(AuthoredEntity->GetUUID());
		PF_CHECK(Tests, CookedBlueprintAfterFirstUpdate.has_value());
		if (CookedBlueprintAfterFirstUpdate)
		{
			const auto Blueprint = CookedBlueprintAfterFirstUpdate->GetBlueprint();
			PF_CHECK(Tests, Blueprint && Blueprint->has_value() &&
				(**Blueprint).FindFloatProperty("runtime_value") == 11.0f);
		}
		const auto AuthoredBlueprintAfterFirstUpdate = AuthoredEntity->GetBlueprint();
		PF_CHECK(Tests, AuthoredBlueprintAfterFirstUpdate && AuthoredBlueprintAfterFirstUpdate->has_value() &&
			(**AuthoredBlueprintAfterFirstUpdate).FindFloatProperty("runtime_value") == 10.0f);
		++BlueprintProgramGeneration;
		Advanced = Session.Advance(Timestep(1.0 / 60.0));
		PF_CHECK(Tests, Advanced.has_value());
		const auto SecondRuntimePose = Session.GetActiveScene()->GetRuntimeSkeletalPose(AuthoredEntity->GetUUID());
		PF_CHECK(Tests, SecondRuntimePose.has_value() && SecondRuntimePose->size() == 2);
		if (SecondRuntimePose && SecondRuntimePose->size() == 2)
			PF_CHECK(Tests, std::abs((*SecondRuntimePose)[1][13] - (4.0f / 60.0f)) < 1.0e-4f);
		PF_CHECK(Tests, !AuthoredScene.GetRuntimeSkeletalPose(AuthoredEntity->GetUUID()).has_value());
		ReadRuntimeValue(11.0f);
		const auto CookedBlueprintAfterRebind = Session.GetActiveScene()->FindEntity(AuthoredEntity->GetUUID());
		PF_CHECK(Tests, CookedBlueprintAfterRebind.has_value());
		if (CookedBlueprintAfterRebind)
		{
			const auto Blueprint = CookedBlueprintAfterRebind->GetBlueprint();
			PF_CHECK(Tests, Blueprint && Blueprint->has_value() &&
				(**Blueprint).FindFloatProperty("runtime_value") == 11.0f);
		}
		const auto AuthoredBlueprintAfterRebind = AuthoredEntity->GetBlueprint();
		PF_CHECK(Tests, AuthoredBlueprintAfterRebind && AuthoredBlueprintAfterRebind->has_value() &&
			(**AuthoredBlueprintAfterRebind).FindFloatProperty("runtime_value") == 10.0f);
		PF_CHECK(Tests, CountScriptObservations(ScriptObservations, ScriptRuntimeObservationKind::CallbackCompleted,
			*ScriptIdentifier, AuthoredEntity->GetUUID(), ScriptRuntimeCallback::OnCreate) == 1);
		const auto CookedEntity = Session.GetActiveScene()->FindEntity(AuthoredEntity->GetUUID());
		PF_CHECK(Tests, CookedEntity.has_value());
		PF_CHECK(Tests, CookedEntity && CookedEntity->GetTransform()->Translation.x > 0.0f);
		PF_CHECK(Tests, CookedEntity && CookedEntity->GetTransform()->Translation.y < 0.0f);
		const auto SecondRuntimeEntity = Session.GetActiveScene()->FindEntity(SecondSkeletalEntity->GetUUID());
		PF_CHECK(Tests, SecondRuntimeEntity.has_value());
		const auto AuthoredTransformBeforePlaybackControls = AuthoredEntity->GetTransform();
		if (SecondRuntimeEntity)
		{
			const auto PlaybackBeforePause = Session.GetSkeletalAnimationPlayback(*CookedEntity);
			PF_CHECK(Tests, PlaybackBeforePause && PlaybackBeforePause->CurrentClipKey == AnimationClipKey);
			PF_CHECK(Tests, Session.SetSkeletalAnimationPaused(*CookedEntity, true).has_value());
			const auto PausedPlayback = Session.GetSkeletalAnimationPlayback(*CookedEntity);
			PF_CHECK(Tests, PausedPlayback && PausedPlayback->Paused &&
				PlaybackBeforePause && std::abs(PausedPlayback->TimeSeconds - PlaybackBeforePause->TimeSeconds) < 1.0e-4f);
			const auto PausedFrame = Session.Advance(Timestep(0.25));
			PF_CHECK(Tests, PausedFrame.has_value());
			auto FirstPausedPose = Session.GetActiveScene()->GetRuntimeSkeletalPose(CookedEntity->GetUUID());
			auto SecondIndependentPose = Session.GetActiveScene()->GetRuntimeSkeletalPose(SecondRuntimeEntity->GetUUID());
			PF_CHECK(Tests, FirstPausedPose && SecondIndependentPose &&
				std::abs((*FirstPausedPose)[1][13] - (4.0f / 60.0f)) < 1.0e-4f &&
				std::abs((*SecondIndependentPose)[1][13] - (4.0f / 60.0f + 0.5f)) < 1.0e-4f);

			const auto OtherPlaybackBeforeGlobalPause = Session.GetSkeletalAnimationPlayback(*SecondRuntimeEntity);
			const auto OtherPoseBeforeGlobalPause = Session.GetActiveScene()->GetRuntimeSkeletalPose(SecondRuntimeEntity->GetUUID());
			const size_t ScriptCallbacksBeforeScrub = CountScriptObservations(
				ScriptObservations, ScriptRuntimeObservationKind::CallbackCompleted, *ScriptIdentifier,
				AuthoredEntity->GetUUID(), ScriptRuntimeCallback::OnUpdate);
			PF_CHECK(Tests, Session.Pause().has_value() && Session.IsPaused());
			PF_CHECK(Tests, Session.SeekSkeletalAnimation(*CookedEntity, 0.75f).has_value());
			PF_CHECK(Tests, Session.IsPaused());
			const auto SoughtPose = Session.GetActiveScene()->GetRuntimeSkeletalPose(CookedEntity->GetUUID());
			const auto OtherPlaybackAfterScrub = Session.GetSkeletalAnimationPlayback(*SecondRuntimeEntity);
			const auto OtherPoseAfterScrub = Session.GetActiveScene()->GetRuntimeSkeletalPose(SecondRuntimeEntity->GetUUID());
			PF_CHECK(Tests, SoughtPose && std::abs((*SoughtPose)[1][13] - 0.75f) < 1.0e-4f);
			PF_CHECK(Tests, OtherPlaybackBeforeGlobalPause && OtherPlaybackAfterScrub &&
				std::abs(OtherPlaybackBeforeGlobalPause->TimeSeconds - OtherPlaybackAfterScrub->TimeSeconds) < 1.0e-6f);
			PF_CHECK(Tests, OtherPoseBeforeGlobalPause && OtherPoseAfterScrub &&
				(*OtherPoseBeforeGlobalPause)[1] == (*OtherPoseAfterScrub)[1]);
			PF_CHECK(Tests, CountScriptObservations(ScriptObservations, ScriptRuntimeObservationKind::CallbackCompleted,
				*ScriptIdentifier, AuthoredEntity->GetUUID(), ScriptRuntimeCallback::OnUpdate) == ScriptCallbacksBeforeScrub);
			PF_CHECK(Tests, Session.Resume().has_value() && !Session.IsPaused());
			PF_CHECK(Tests, Session.Advance(Timestep(0.25)).has_value());
			const auto ResumedSoughtPose = Session.GetActiveScene()->GetRuntimeSkeletalPose(CookedEntity->GetUUID());
			PF_CHECK(Tests, ResumedSoughtPose && std::abs((*ResumedSoughtPose)[1][13] - 0.75f) < 1.0e-4f);
			const auto SoughtEvents = Session.TakeSkeletalAnimationEvents();
			PF_CHECK(Tests, std::ranges::none_of(SoughtEvents,
				[&](const SkeletalAnimationEvent& Event) { return Event.Target == *CookedEntity; }));
			PF_CHECK(Tests, Session.SetSkeletalAnimationPaused(*CookedEntity, false).has_value());
			PF_CHECK(Tests, Session.TransitionSkeletalAnimation(
				*CookedEntity, "animation/cccccccccccccccccccccccccccccccc", 0.5f).has_value());
			PF_CHECK(Tests, Session.Advance(Timestep(0.25)).has_value());
			const auto CrossfadeEvents = Session.TakeSkeletalAnimationEvents();
			PF_CHECK(Tests, std::ranges::any_of(CrossfadeEvents, [&](const SkeletalAnimationEvent& Event)
				{ return Event.Target == *CookedEntity && Event.ClipKey == "animation/cccccccccccccccccccccccccccccccc" &&
					Event.MarkerKey == "event/second-clip-half" && Event.Name == "SecondClipHalf"; }));
			const auto HalfTransitionPose = Session.GetActiveScene()->GetRuntimeSkeletalPose(CookedEntity->GetUUID());
			PF_CHECK(Tests, HalfTransitionPose && std::abs((*HalfTransitionPose)[1][13] - 0.25f) < 1.0e-4f);
			PF_CHECK(Tests, Session.Advance(Timestep(0.25)).has_value());
			const auto CompletedTransitionPose = Session.GetActiveScene()->GetRuntimeSkeletalPose(CookedEntity->GetUUID());
			PF_CHECK(Tests, CompletedTransitionPose && std::abs((*CompletedTransitionPose)[1][13] + 0.5f) < 1.0e-4f);
			PF_CHECK(Tests, Session.PlaySkeletalAnimation(*CookedEntity, AnimationClipKey, true).has_value());
			PF_CHECK(Tests, Session.Advance(Timestep(0.25)).has_value());
			const auto ReplayedPose = Session.GetActiveScene()->GetRuntimeSkeletalPose(CookedEntity->GetUUID());
			PF_CHECK(Tests, ReplayedPose && std::abs((*ReplayedPose)[1][13] - 0.5f) < 1.0e-4f);
			PF_CHECK(Tests, Session.SeekSkeletalAnimation(*CookedEntity, 0.85f).has_value());
			PF_CHECK(Tests, Session.Advance(Timestep(0.2)).has_value());
			const auto LoopEvents = Session.TakeSkeletalAnimationEvents();
			std::vector<std::string> FirstActorLoopMarkers;
			for (const SkeletalAnimationEvent& Event : LoopEvents)
				if (Event.Target == *CookedEntity) FirstActorLoopMarkers.push_back(Event.MarkerKey);
			const std::vector<std::string> ExpectedLoopMarkers{ "event/first-clip-end", "event/first-clip-loop" };
			PF_CHECK(Tests, FirstActorLoopMarkers == ExpectedLoopMarkers);
			PF_CHECK(Tests, std::ranges::all_of(LoopEvents, [](const SkeletalAnimationEvent& Event)
				{ return Event.Target.IsValid() && Event.SessionGeneration != 0; }));
			const auto AuthoredTransformAfterPlaybackControls = AuthoredEntity->GetTransform();
			PF_CHECK(Tests, AuthoredTransformBeforePlaybackControls && AuthoredTransformAfterPlaybackControls &&
				AuthoredTransformBeforePlaybackControls->Translation == AuthoredTransformAfterPlaybackControls->Translation &&
				AuthoredTransformBeforePlaybackControls->Rotation == AuthoredTransformAfterPlaybackControls->Rotation &&
				AuthoredTransformBeforePlaybackControls->Scale == AuthoredTransformAfterPlaybackControls->Scale &&
				!AuthoredScene.GetRuntimeSkeletalPose(AuthoredEntity->GetUUID()));
		}
		Session.Stop();
		PF_CHECK(Tests, Session.TakeSkeletalAnimationEvents().empty());
		PF_CHECK(Tests, !Session.PlaySkeletalAnimation(*CookedEntity, AnimationClipKey));
	}
} // namespace

void RunPlayerSessionTests(TestRunner& Tests)
{
	TestPlayerSessionLifecycleAndTransitions(Tests);
	TestSourcePlayerRejectsStaleBlueprintGraphPair(Tests);
	TestCookedPlayerSessionDoesNotOpenSourceProject(Tests);
}
