#include "TestSupport.h"

#include "Assets/AssetTypes.h"
#include "Assets/AssetOperations.h"
#include "Assets/Project.h"
#include "Assets/SceneAssetService.h"
#include "Assets/CookedAssetCatalog.h"
#include "Runtime/CookedContentProvider.h"
#include "Runtime/PlayerSession.h"
#include "Scene/Components/AudioSourceComponent.h"
#include "Scene/Components/BoxColliderComponent.h"
#include "Scene/Components/RigidbodyComponent.h"
#include "Scene/SceneSerializer.h"
#include "Scene/Components/ScriptComponent.h"
#include "Scene/Scene.h"
#include "Scripting/ScriptRuntime.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <utility>

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
		std::array<PulseForge::ScriptRuntimeObservation, 64> Events{};
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
		auto RetryAsset = SceneAssetService::Create(
			CreatedProject->GetAssetRegistry(), CreatedProject->GetRootPath(), "Assets/Retry.scene", RetryScene);
		PF_CHECK(Tests, RetryAsset.has_value());
		if (!RetryAsset) return;
		const auto StartupBytes = ReadFile(Root / "Assets/Startup.scene");
		const auto SecondBytes = ReadFile(Root / "Assets/Second.scene");
		const auto RetryBytes = ReadFile(Root / "Assets/Retry.scene");
		CreatedProject.reset();

		SceneRuntimeDesc RuntimeDescription;
		RuntimeDescription.Audio.OutputBackend = AudioOutputBackend::Null;
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
		PF_CHECK(Tests, Session.GetActiveScene() && Session.GetActiveScene()->GetEntityCount() == 2);
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
		auto AuthoredEntity = AuthoredScene.CreateEntity("Cooked Runtime Entity");
		PF_CHECK(Tests, AuthoredEntity.has_value());
		if (!AuthoredEntity) return;
		PF_CHECK(Tests, AuthoredEntity->SetScript(ScriptComponent{ *ScriptIdentifier }).has_value());
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
		const CookedAssetDigest Digest = CookedAssetCatalog::ComputePayloadDigest(Payload);
		CookedAssetEntry Entry{ *SceneIdentifier, CookedAssetProductKind::Scene, {}, "products/startup.scene", 1,
			Payload.size(), Digest.Algorithm, Digest.Value, {} };
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
			{ std::move(Entry), std::move(ScriptEntry), std::move(AudioEntry) });
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
		SceneRuntimeServices RuntimeServices;
		RuntimeServices.ScriptObserver = { &ScriptObservations, StoreScriptObservation };
		SceneRuntimeDesc RuntimeDescription;
		RuntimeDescription.Audio.OutputBackend = AudioOutputBackend::Null;
		PlayerSession Session(*Provider, RuntimeDescription, RuntimeServices);
		PF_CHECK(Tests, Session.Start().has_value());
		PF_CHECK(Tests, Session.IsCookedSession());
		PF_CHECK(Tests, Session.GetProject() == nullptr);
		PF_CHECK(Tests, !std::filesystem::exists(Root / "SourceProject.pfproj"));
		PF_CHECK(Tests, !std::filesystem::exists(Root / "Assets"));
		PF_CHECK(Tests, Session.GetActiveSceneAsset() == *SceneIdentifier);
		PF_CHECK(Tests, Session.GetActiveScene() && Session.GetActiveScene()->GetEntityCount() == 1);
		auto Advanced = Session.Advance(Timestep(1.0 / 60.0));
		PF_CHECK(Tests, Advanced.has_value());
		PF_CHECK(Tests, Advanced && Advanced->PhysicsSteps > 0);
		PF_CHECK(Tests, CountScriptObservations(ScriptObservations, ScriptRuntimeObservationKind::CallbackCompleted,
			*ScriptIdentifier, AuthoredEntity->GetUUID(), ScriptRuntimeCallback::OnCreate) == 1);
		const auto CookedEntity = Session.GetActiveScene()->FindEntity(AuthoredEntity->GetUUID());
		PF_CHECK(Tests, CookedEntity.has_value());
		PF_CHECK(Tests, CookedEntity && CookedEntity->GetTransform()->Translation.x > 0.0f);
		PF_CHECK(Tests, CookedEntity && CookedEntity->GetTransform()->Translation.y < 0.0f);
		Session.Stop();
	}
} // namespace

void RunPlayerSessionTests(TestRunner& Tests)
{
	TestPlayerSessionLifecycleAndTransitions(Tests);
	TestCookedPlayerSessionDoesNotOpenSourceProject(Tests);
}
