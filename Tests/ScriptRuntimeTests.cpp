#include "TestEngineSupport.h"
#include "Core/Diagnostics.h"
#include "Input/GameplayInput.h"

namespace
{
	std::vector<std::byte> MakeSilentScriptTestWave()
	{
		constexpr uint32_t SampleCount = 44100 * 5;
		constexpr uint32_t DataSize = SampleCount * 2;
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

	void TestScriptRuntime(TestRunner& Tests)
	{
		using namespace PulseForge;
		std::error_code FileError;
		const std::filesystem::path TemporaryDirectory = std::filesystem::temp_directory_path(FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError) return;

		const auto ProjectIdentifier = UUID::Generate();
		PF_CHECK(Tests, ProjectIdentifier.has_value());
		if (!ProjectIdentifier) return;

		const std::filesystem::path ProjectRoot = TemporaryDirectory / ("PulseForgeScriptRuntime-" + ProjectIdentifier->ToString());
		struct ProjectCleanup
		{
			std::filesystem::path Path;
			~ProjectCleanup()
			{
				std::error_code Error;
				std::filesystem::remove_all(Path, Error);
			}
		} Cleanup{ ProjectRoot };

		auto ProjectResult = Project::Create(ProjectRoot / "ScriptProject.pfproj", "Script Runtime Tests");
		PF_CHECK(Tests, ProjectResult.has_value());
		if (!ProjectResult) return;
		std::filesystem::create_directories(ProjectRoot / "Assets/Scripts", FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError) return;

		const auto CreateScriptAsset = [&](std::string_view FileName, std::string_view Source)
		{
			const auto Contents = std::as_bytes(std::span(Source.data(), Source.size()));
			return AssetOperations::CreateAssetFromBytes(ProjectResult->GetAssetRegistry(), ProjectResult->GetRootPath(), Contents,
				std::filesystem::path("Assets/Scripts") / FileName);
		};

		constexpr std::string_view TransformScript = R"(
local elapsed = 0
function OnCreate()
  assert(io == nil and os == nil and debug == nil and package == nil)
  assert(load == nil and loadfile == nil and dofile == nil and collectgarbage == nil)
  assert(pcall == nil and xpcall == nil)
  assert(string.dump == nil)
  local found = scene.find_entity(entity:uuid())
  assert(found ~= nil and found:uuid() == entity:uuid())
end
function OnUpdate(dt)
  elapsed = elapsed + dt
  assert(entity:set_translation(elapsed, 2, 3))
end
function OnDestroy()
  assert(entity:set_translation(-1, 2, 3))
end
)";
		const auto TransformScriptAsset = CreateScriptAsset("transform.LUA", TransformScript);
		PF_CHECK(Tests, TransformScriptAsset.has_value());
		if (!TransformScriptAsset) return;

		const auto RelativeMotionScriptAsset = CreateScriptAsset("relative_motion.lua",
			"local elapsed = 0\n"
			"local initialX, initialY, initialZ = 0, 0, 0\n"
			"function OnCreate()\n"
			"  initialX, initialY, initialZ = entity:get_translation()\n"
			"  assert(initialX ~= nil and initialY ~= nil and initialZ ~= nil)\n"
			"end\n"
			"function OnUpdate(dt)\n"
			"  elapsed = elapsed + dt\n"
			"  assert(entity:set_translation(initialX + math.sin(elapsed) * 0.25, initialY, initialZ))\n"
			"end\n");
		PF_CHECK(Tests, RelativeMotionScriptAsset.has_value());
		if (!RelativeMotionScriptAsset) return;

		Scene RelativeMotionScene;
		const auto OriginalMover = RelativeMotionScene.CreateEntity("Original mover");
		PF_CHECK(Tests, OriginalMover.has_value());
		if (!OriginalMover) return;
		TransformComponent OriginalTransform;
		OriginalTransform.Translation = { 3.0f, 2.0f, -4.0f };
		PF_CHECK(Tests, OriginalMover->SetTransform(OriginalTransform).has_value());
		PF_CHECK(Tests, OriginalMover->SetScript(ScriptComponent{ RelativeMotionScriptAsset->ID }).has_value());
		const auto DuplicatedMover = RelativeMotionScene.DuplicateEntity(*OriginalMover);
		PF_CHECK(Tests, DuplicatedMover.has_value());
		if (!DuplicatedMover) return;
		TransformComponent DuplicatedTransform;
		DuplicatedTransform.Translation = { -7.0f, 5.0f, 11.0f };
		PF_CHECK(Tests, DuplicatedMover->SetTransform(DuplicatedTransform).has_value());
		ScriptRuntime RelativeMotionRuntime(*ProjectResult);
		PF_CHECK(Tests, RelativeMotionRuntime.Start(RelativeMotionScene).has_value());
		PF_CHECK(Tests, RelativeMotionRuntime.Advance(RelativeMotionScene, Timestep(0.5)).has_value());
		const auto OriginalMovedTransform = OriginalMover->GetTransform();
		const auto DuplicatedMovedTransform = DuplicatedMover->GetTransform();
		const float ExpectedOscillation = std::sin(0.5f) * 0.25f;
		PF_CHECK(Tests, OriginalMovedTransform &&
							std::abs(OriginalMovedTransform->Translation.x - (3.0f + ExpectedOscillation)) < 0.0001f &&
							std::abs(OriginalMovedTransform->Translation.y - 2.0f) < 0.0001f &&
							std::abs(OriginalMovedTransform->Translation.z + 4.0f) < 0.0001f);
		PF_CHECK(Tests, DuplicatedMovedTransform &&
							std::abs(DuplicatedMovedTransform->Translation.x - (-7.0f + ExpectedOscillation)) < 0.0001f &&
							std::abs(DuplicatedMovedTransform->Translation.y - 5.0f) < 0.0001f &&
							std::abs(DuplicatedMovedTransform->Translation.z - 11.0f) < 0.0001f);
		PF_CHECK(Tests, RelativeMotionRuntime.GetDiagnostics().empty());
		RelativeMotionRuntime.Stop();

		std::filesystem::create_directories(ProjectRoot / "Assets/Prefabs", FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError) return;
		const auto PrefabChildScriptAsset =
			CreateScriptAsset("prefab_child.lua", "function OnCreate() assert(entity:set_translation(9, 8, 7)) end\n");
		PF_CHECK(Tests, PrefabChildScriptAsset.has_value());
		if (!PrefabChildScriptAsset) return;

		Scene ScriptSpawnPrefabSource;
		const auto ScriptSpawnPrefabRoot = ScriptSpawnPrefabSource.CreateEntity("Script-spawned prefab root");
		const auto ScriptSpawnPrefabChild = ScriptSpawnPrefabSource.CreateEntity("Script-spawned prefab child");
		PF_CHECK(Tests, ScriptSpawnPrefabRoot && ScriptSpawnPrefabChild);
		if (!ScriptSpawnPrefabRoot || !ScriptSpawnPrefabChild) return;
		PF_CHECK(Tests, ScriptSpawnPrefabChild->SetParent(*ScriptSpawnPrefabRoot).has_value());
		PF_CHECK(Tests, ScriptSpawnPrefabChild->SetScript(ScriptComponent{ PrefabChildScriptAsset->ID }).has_value());
		const auto ScriptSpawnPrefab = PrefabAssetService::Create(ProjectResult->GetAssetRegistry(), ProjectResult->GetRootPath(),
			"Assets/Prefabs/ScriptSpawn.prefab", ScriptSpawnPrefabSource, *ScriptSpawnPrefabRoot);
		PF_CHECK(Tests, ScriptSpawnPrefab.has_value());
		if (!ScriptSpawnPrefab) return;

		const std::string SceneMutationScript = "local CreatedEntity\n"
												"local SpawnedPrefab\n"
												"local Destroyed = false\n"
												"function OnCreate()\n"
												"  local invalidName, nameMessage = scene.create_entity(17)\n"
												"  assert(invalidName == nil and nameMessage ~= nil)\n"
												"  CreatedEntity, nameMessage = scene.create_entity('Lua-created entity')\n"
												"  assert(CreatedEntity, nameMessage)\n"
												"  local found = scene.find_entity(CreatedEntity:uuid())\n"
												"  assert(found ~= nil and found:uuid() == CreatedEntity:uuid())\n"
												"  local invalidPrefab, prefabMessage = scene.spawn_prefab('not-a-uuid')\n"
												"  assert(invalidPrefab == nil and prefabMessage ~= nil)\n"
												"  local wrongType = scene.spawn_prefab('" +
												TransformScriptAsset->ID.ToString() +
												"')\n"
												"  assert(wrongType == nil)\n"
												"  SpawnedPrefab, prefabMessage = scene.spawn_prefab('" +
												ScriptSpawnPrefab->ID.ToString() +
												"')\n"
												"  assert(SpawnedPrefab, prefabMessage)\n"
												"end\n"
												"function OnUpdate()\n"
												"  if not Destroyed then\n"
												"    assert(CreatedEntity:destroy())\n"
												"    Destroyed = true\n"
												"    assert(scene.find_entity(CreatedEntity:uuid()) == nil)\n"
												"    local x, staleMessage = CreatedEntity:get_translation()\n"
												"    assert(x == nil and staleMessage ~= nil)\n"
												"    local transient, message = scene.create_entity('Transient Lua entity')\n"
												"    assert(transient, message)\n"
												"    assert(transient:destroy())\n"
												"    assert(entity:destroy())\n"
												"  end\n"
												"  assert(SpawnedPrefab:set_translation(4, 5, 6))\n"
												"end\n"
												"function OnDestroy()\n"
												"  local x, staleMessage = entity:get_translation()\n"
												"  assert(x == nil and staleMessage ~= nil)\n"
												"end\n";
		const auto SceneMutationScriptAsset = CreateScriptAsset("scene_mutation.lua", SceneMutationScript);
		PF_CHECK(Tests, SceneMutationScriptAsset.has_value());
		if (!SceneMutationScriptAsset) return;

		Scene SceneMutationTestScene;
		const auto SceneMutationController = SceneMutationTestScene.CreateEntity("Scene mutation controller");
		PF_CHECK(Tests, SceneMutationController.has_value());
		if (!SceneMutationController) return;
		PF_CHECK(Tests, SceneMutationController->SetScript(ScriptComponent{ SceneMutationScriptAsset->ID }).has_value());
		ScriptRuntime SceneMutationRuntime(*ProjectResult);
		PF_CHECK(Tests, SceneMutationRuntime.Start(SceneMutationTestScene).has_value());
		PF_CHECK(Tests, SceneMutationRuntime.GetDiagnostics().empty());
		PF_CHECK(Tests, SceneMutationTestScene.GetEntityCount() == 4);

		std::optional<Entity> SpawnedPrefabRoot;
		std::optional<Entity> SpawnedPrefabChild;
		for (const Entity& Current : SceneMutationTestScene.GetEntities())
		{
			const auto Tag = Current.GetTag();
			if (Tag && Tag->Name == "Script-spawned prefab root") SpawnedPrefabRoot = Current;
			else if (Tag && Tag->Name == "Script-spawned prefab child") SpawnedPrefabChild = Current;
		}
		PF_CHECK(Tests, SpawnedPrefabRoot.has_value() && SpawnedPrefabChild.has_value());
		if (SpawnedPrefabRoot && SpawnedPrefabChild)
		{
			const auto SpawnedChildParent = SpawnedPrefabChild->GetParent();
			PF_CHECK(Tests,
				SpawnedChildParent && SpawnedChildParent->has_value() && (**SpawnedChildParent).GetUUID() == SpawnedPrefabRoot->GetUUID());
		}

		PF_CHECK(Tests, SceneMutationRuntime.Advance(SceneMutationTestScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, SceneMutationTestScene.GetEntityCount() == 2);
		if (SpawnedPrefabRoot)
		{
			const auto SpawnedTransform = SpawnedPrefabRoot->GetTransform();
			PF_CHECK(Tests, SpawnedTransform && std::abs(SpawnedTransform->Translation.x - 4.0f) < 0.0001f &&
								std::abs(SpawnedTransform->Translation.y - 5.0f) < 0.0001f &&
								std::abs(SpawnedTransform->Translation.z - 6.0f) < 0.0001f);
		}
		PF_CHECK(Tests, SceneMutationRuntime.GetDiagnostics().empty());
		PF_CHECK(Tests, SceneMutationRuntime.GetScriptCount() == 2);
		if (SpawnedPrefabChild)
		{
			const auto SpawnedChildTransform = SpawnedPrefabChild->GetTransform();
			PF_CHECK(Tests, SpawnedChildTransform && std::abs(SpawnedChildTransform->Translation.x - 9.0f) < 0.0001f &&
								std::abs(SpawnedChildTransform->Translation.y - 8.0f) < 0.0001f &&
								std::abs(SpawnedChildTransform->Translation.z - 7.0f) < 0.0001f);
		}
		PF_CHECK(Tests, SceneMutationRuntime.Advance(SceneMutationTestScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, SceneMutationRuntime.GetScriptCount() == 1);
		PF_CHECK(Tests, SceneMutationRuntime.GetDiagnostics().empty());
		SceneMutationRuntime.Stop();

		const UUID LifecycleControllerID{ 0x5500000000000000ull, 1 };
		const UUID LifecycleVictimID{ 0x5500000000000000ull, 2 };
		const auto DestroyLaterScript = CreateScriptAsset("destroy_later.lua", "function OnUpdate()\n"
																			   "  local victim = scene.find_entity('" +
																				   LifecycleVictimID.ToString() +
																				   "')\n"
																				   "  if victim then assert(victim:destroy()) end\n"
																				   "end\n");
		const auto LifecycleVictimScript =
			CreateScriptAsset("lifecycle_victim.lua", "function OnUpdate()\n"
													  "  local marker, message = scene.create_entity('victim-updated')\n"
													  "  assert(marker, message)\n"
													  "end\n"
													  "function OnDestroy()\n"
													  "  local marker, message = scene.create_entity('victim-destroyed')\n"
													  "  assert(marker, message)\n"
													  "end\n");
		PF_CHECK(Tests, DestroyLaterScript && LifecycleVictimScript);
		if (!DestroyLaterScript || !LifecycleVictimScript) return;
		Scene CallbackMutationScene;
		const auto LifecycleController = CallbackMutationScene.CreateEntityWithUUID(LifecycleControllerID, "Lifecycle controller");
		const auto LifecycleVictim = CallbackMutationScene.CreateEntityWithUUID(LifecycleVictimID, "Lifecycle victim");
		PF_CHECK(Tests, LifecycleController && LifecycleVictim);
		if (!LifecycleController || !LifecycleVictim) return;
		PF_CHECK(Tests, LifecycleController->SetScript(ScriptComponent{ DestroyLaterScript->ID }).has_value());
		PF_CHECK(Tests, LifecycleVictim->SetScript(ScriptComponent{ LifecycleVictimScript->ID }).has_value());
		ScriptRuntime CallbackMutationRuntime(*ProjectResult);
		PF_CHECK(Tests, CallbackMutationRuntime.Start(CallbackMutationScene).has_value());
		PF_CHECK(Tests, CallbackMutationRuntime.GetScriptCount() == 2);
		PF_CHECK(Tests, CallbackMutationRuntime.Advance(CallbackMutationScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, !CallbackMutationScene.FindEntity(LifecycleVictimID).has_value());
		PF_CHECK(Tests, CallbackMutationRuntime.GetScriptCount() == 1);
		bool SawVictimDestroyed = false;
		bool SawVictimUpdated = false;
		for (const Entity& Current : CallbackMutationScene.GetEntities())
		{
			const auto Tag = Current.GetTag();
			if (!Tag) continue;
			SawVictimDestroyed = SawVictimDestroyed || Tag->Name == "victim-destroyed";
			SawVictimUpdated = SawVictimUpdated || Tag->Name == "victim-updated";
		}
		PF_CHECK(Tests, SawVictimDestroyed && !SawVictimUpdated);
		PF_CHECK(Tests, CallbackMutationRuntime.GetDiagnostics().empty());
		CallbackMutationRuntime.Stop();

		const UUID StartupControllerID{ 0x5500000000000000ull, 3 };
		const UUID StartupVictimID{ 0x5500000000000000ull, 4 };
		const auto DestroyAtStartupScript =
			CreateScriptAsset("destroy_at_startup.lua", "function OnCreate()\n"
														"  local victim = scene.find_entity('" +
															StartupVictimID.ToString() +
															"')\n"
															"  if victim then assert(victim:destroy()) end\n"
															"end\n");
		PF_CHECK(Tests, DestroyAtStartupScript.has_value());
		if (!DestroyAtStartupScript) return;
		Scene StartupMutationScene;
		const auto StartupController = StartupMutationScene.CreateEntityWithUUID(StartupControllerID, "Startup controller");
		const auto StartupVictim = StartupMutationScene.CreateEntityWithUUID(StartupVictimID, "Startup victim");
		PF_CHECK(Tests, StartupController && StartupVictim);
		if (!StartupController || !StartupVictim) return;
		PF_CHECK(Tests, StartupController->SetScript(ScriptComponent{ DestroyAtStartupScript->ID }).has_value());
		PF_CHECK(Tests, StartupVictim->SetScript(ScriptComponent{ LifecycleVictimScript->ID }).has_value());
		ScriptRuntime StartupMutationRuntime(*ProjectResult);
		PF_CHECK(Tests, StartupMutationRuntime.Start(StartupMutationScene).has_value());
		PF_CHECK(Tests, !StartupMutationScene.FindEntity(StartupVictimID).has_value());
		PF_CHECK(Tests, StartupMutationRuntime.GetScriptCount() == 1);
		PF_CHECK(Tests, StartupMutationRuntime.GetDiagnostics().empty());
		StartupMutationRuntime.Stop();

		const UUID ReusedScriptEntityID{ 0x5500000000000000ull, 5 };
		const auto OldIncarnationScript =
			CreateScriptAsset("old_incarnation.lua", "function OnDestroy()\n"
													 "  local replacement = scene.find_entity(entity:uuid())\n"
													 "  if replacement then assert(replacement:set_translation(99, 0, 0)) end\n"
													 "  local marker, message = scene.create_entity('old-incarnation-destroyed')\n"
													 "  assert(marker, message)\n"
													 "end\n");
		const auto ReplacementIncarnationScript =
			CreateScriptAsset("replacement_incarnation.lua", "local updates = 0\n"
															 "function OnCreate()\n"
															 "  local x = entity:get_translation()\n"
															 "  assert(x == 10)\n"
															 "end\n"
															 "function OnUpdate()\n"
															 "  updates = updates + 1\n"
															 "  assert(entity:set_translation(updates, 0, 0))\n"
															 "end\n");
		PF_CHECK(Tests, OldIncarnationScript && ReplacementIncarnationScript);
		if (!OldIncarnationScript || !ReplacementIncarnationScript) return;
		Scene ReusedScriptScene;
		const auto OldIncarnation = ReusedScriptScene.CreateEntityWithUUID(ReusedScriptEntityID, "Old script incarnation");
		PF_CHECK(Tests, OldIncarnation.has_value());
		if (!OldIncarnation) return;
		PF_CHECK(Tests, OldIncarnation->SetScript(ScriptComponent{ OldIncarnationScript->ID }).has_value());
		ScriptRuntime ReusedScriptRuntime(*ProjectResult);
		PF_CHECK(Tests, ReusedScriptRuntime.Start(ReusedScriptScene).has_value());
		PF_CHECK(Tests, ReusedScriptRuntime.Advance(ReusedScriptScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, ReusedScriptScene.DestroyEntity(*OldIncarnation).has_value());
		const auto NewIncarnation = ReusedScriptScene.CreateEntityWithUUID(ReusedScriptEntityID, "New script incarnation");
		PF_CHECK(Tests, NewIncarnation.has_value());
		if (!NewIncarnation) return;
		TransformComponent NewIncarnationTransform;
		NewIncarnationTransform.Translation.x = 10.0f;
		PF_CHECK(Tests, NewIncarnation->SetTransform(NewIncarnationTransform).has_value());
		PF_CHECK(Tests, NewIncarnation->SetScript(ScriptComponent{ ReplacementIncarnationScript->ID }).has_value());
		PF_CHECK(Tests, ReusedScriptRuntime.Advance(ReusedScriptScene, Timestep(1.0 / 60.0)).has_value());
		const auto NewIncarnationResult = NewIncarnation->GetTransform();
		PF_CHECK(Tests, NewIncarnationResult && std::abs(NewIncarnationResult->Translation.x - 1.0f) < 0.0001f);
		PF_CHECK(Tests, ReusedScriptScene.GetEntityCount() == 2);
		PF_CHECK(Tests, ReusedScriptRuntime.GetScriptCount() == 1);
		PF_CHECK(Tests, ReusedScriptRuntime.GetDiagnostics().empty());
		ReusedScriptRuntime.Stop();

		const UUID FailedThenReusedID{ 0x5500000000000000ull, 6 };
		const UUID AttachmentBlockerID{ 0x5500000000000000ull, 7 };
		const auto RetryOnNewIncarnationScript =
			CreateScriptAsset("retry_new_incarnation.lua", "function OnCreate()\n"
														   "  if scene.find_entity('" +
															   AttachmentBlockerID.ToString() +
															   "') then error('blocked attachment') end\n"
															   "end\n"
															   "function OnUpdate() assert(entity:set_translation(7, 0, 0)) end\n");
		PF_CHECK(Tests, RetryOnNewIncarnationScript.has_value());
		if (!RetryOnNewIncarnationScript) return;
		Scene FailedAttachmentScene;
		const auto AttachmentBlocker = FailedAttachmentScene.CreateEntityWithUUID(AttachmentBlockerID, "Attachment blocker");
		const auto FailedIncarnation = FailedAttachmentScene.CreateEntityWithUUID(FailedThenReusedID, "Failed incarnation");
		PF_CHECK(Tests, AttachmentBlocker && FailedIncarnation);
		if (!AttachmentBlocker || !FailedIncarnation) return;
		PF_CHECK(Tests, FailedIncarnation->SetScript(ScriptComponent{ RetryOnNewIncarnationScript->ID }).has_value());
		ScriptRuntime FailedAttachmentRuntime(*ProjectResult);
		PF_CHECK(Tests, FailedAttachmentRuntime.Start(FailedAttachmentScene).has_value());
		PF_CHECK(Tests, FailedAttachmentRuntime.GetScriptCount() == 0);
		PF_CHECK(Tests, FailedAttachmentRuntime.GetDiagnostics().size() == 1);
		FailedAttachmentRuntime.ClearDiagnostics();
		PF_CHECK(Tests, FailedAttachmentScene.DestroyEntity(*AttachmentBlocker).has_value());
		PF_CHECK(Tests, FailedAttachmentScene.DestroyEntity(*FailedIncarnation).has_value());
		const auto ReusedFailedIncarnation = FailedAttachmentScene.CreateEntityWithUUID(FailedThenReusedID, "Recovered incarnation");
		PF_CHECK(Tests, ReusedFailedIncarnation.has_value());
		if (!ReusedFailedIncarnation) return;
		PF_CHECK(Tests, ReusedFailedIncarnation->SetScript(ScriptComponent{ RetryOnNewIncarnationScript->ID }).has_value());
		PF_CHECK(Tests, FailedAttachmentRuntime.Advance(FailedAttachmentScene, Timestep(1.0 / 60.0)).has_value());
		const auto RecoveredTransform = ReusedFailedIncarnation->GetTransform();
		PF_CHECK(Tests, RecoveredTransform && std::abs(RecoveredTransform->Translation.x - 7.0f) < 0.0001f);
		PF_CHECK(Tests, FailedAttachmentRuntime.GetScriptCount() == 1);
		PF_CHECK(Tests, FailedAttachmentRuntime.GetDiagnostics().empty());
		FailedAttachmentRuntime.Stop();

		Scene TestScene;
		const auto FirstEntity = TestScene.CreateEntity("First scripted entity");
		const auto SecondEntity = TestScene.CreateEntity("Second scripted entity");
		const auto DisabledEntity = TestScene.CreateEntity("Disabled scripted entity");
		PF_CHECK(Tests, FirstEntity && SecondEntity && DisabledEntity);
		if (!FirstEntity || !SecondEntity || !DisabledEntity) return;
		PF_CHECK(Tests, FirstEntity->SetScript(ScriptComponent{ TransformScriptAsset->ID }).has_value());
		PF_CHECK(Tests, SecondEntity->SetScript(ScriptComponent{ TransformScriptAsset->ID }).has_value());
		PF_CHECK(Tests, DisabledEntity->SetScript(ScriptComponent{ TransformScriptAsset->ID, false }).has_value());

		ScriptRuntime Runtime(*ProjectResult);
		const auto BeforeStart = Runtime.Advance(TestScene, Timestep(0.1));
		PF_CHECK(Tests, !BeforeStart && BeforeStart.error().Code == ScriptRuntimeErrorCode::NotRunning);
		PF_CHECK(Tests, Runtime.Start(TestScene).has_value());
		PF_CHECK(Tests, Runtime.IsRunning() && Runtime.GetScriptCount() == 2);
		PF_CHECK(Tests, Runtime.GetDiagnostics().empty());
		const auto DuplicateStart = Runtime.Start(TestScene);
		PF_CHECK(Tests, !DuplicateStart && DuplicateStart.error().Code == ScriptRuntimeErrorCode::AlreadyRunning);
		Scene OtherScene;
		const auto DifferentScene = Runtime.Advance(OtherScene, Timestep(0.1));
		PF_CHECK(Tests, !DifferentScene && DifferentScene.error().Code == ScriptRuntimeErrorCode::DifferentScene);
		const auto NegativeDelta = Runtime.Advance(TestScene, Timestep(-0.1));
		PF_CHECK(Tests, !NegativeDelta && NegativeDelta.error().Code == ScriptRuntimeErrorCode::InvalidDeltaTime);
		PF_CHECK(Tests, Runtime.Advance(TestScene, Timestep(0.25)).has_value());
		const auto FirstTransform = FirstEntity->GetTransform();
		const auto SecondTransform = SecondEntity->GetTransform();
		PF_CHECK(Tests, FirstTransform && std::abs(FirstTransform->Translation.x - 0.25f) < 0.0001f);
		PF_CHECK(Tests, SecondTransform && std::abs(SecondTransform->Translation.x - 0.25f) < 0.0001f);

		PF_CHECK(Tests, FirstEntity->RemoveScript().has_value());
		PF_CHECK(Tests, Runtime.Advance(TestScene, Timestep(0.1)).has_value());
		PF_CHECK(Tests, Runtime.GetScriptCount() == 1);
		const auto DetachedTransform = FirstEntity->GetTransform();
		const auto ActiveTransform = SecondEntity->GetTransform();
		PF_CHECK(Tests, DetachedTransform && DetachedTransform->Translation.x == -1.0f);
		PF_CHECK(Tests, ActiveTransform && std::abs(ActiveTransform->Translation.x - 0.35f) < 0.0001f);
		Runtime.Stop();
		PF_CHECK(Tests, !Runtime.IsRunning() && Runtime.GetScriptCount() == 0);
		const auto StoppedTransform = SecondEntity->GetTransform();
		PF_CHECK(Tests, StoppedTransform && StoppedTransform->Translation.x == -1.0f);
		Runtime.Stop();

		Scene CoordinatedScene;
		const auto CoordinatedScriptEntity = CoordinatedScene.CreateEntity("Coordinated script");
		const auto CoordinatedPhysicsEntity = CoordinatedScene.CreateEntity("Coordinated body");
		PF_CHECK(Tests, CoordinatedScriptEntity && CoordinatedPhysicsEntity);
		if (!CoordinatedScriptEntity || !CoordinatedPhysicsEntity) return;
		PF_CHECK(Tests, CoordinatedScriptEntity->SetScript(ScriptComponent{ TransformScriptAsset->ID }).has_value());
		PF_CHECK(Tests, CoordinatedPhysicsEntity->SetRigidbody(RigidbodyComponent{}).has_value());
		PF_CHECK(Tests, CoordinatedPhysicsEntity->SetBoxCollider(BoxColliderComponent{}).has_value());

		SceneRuntimeDesc RuntimeDescription;
		RuntimeDescription.Audio.OutputBackend = AudioOutputBackend::Null;
		RuntimeDescription.Physics.FixedStepSeconds = 0.1;
		RuntimeDescription.Physics.MaxSubsteps = 2;
		RuntimeDescription.Physics.MaxFrameDeltaSeconds = 0.25;
		DiagnosticRecorder RuntimeDiagnostics(32);
		SceneRuntimeServices RuntimeServices{ .Diagnostics = &RuntimeDiagnostics };
		SceneRuntime CoordinatedRuntime(*ProjectResult, RuntimeDescription, RuntimeServices);
		const auto BeforeRuntimeStart = CoordinatedRuntime.Advance(CoordinatedScene, Timestep(0.1));
		PF_CHECK(Tests, !BeforeRuntimeStart && BeforeRuntimeStart.error().Code == SceneRuntimeErrorCode::NotRunning);
		PF_CHECK(Tests, CoordinatedRuntime.Start(CoordinatedScene).has_value());
		PF_CHECK(Tests, CoordinatedRuntime.IsRunning());
		PF_CHECK(Tests, RuntimeDiagnostics.CaptureSnapshot().Events.empty());
		SceneRuntime* const RunningRuntimeIdentity = &CoordinatedRuntime;
		RuntimeDiagnostics.SetCaptureEnabled(true);
		const auto DuplicateRuntimeStart = CoordinatedRuntime.Start(CoordinatedScene);
		PF_CHECK(Tests, !DuplicateRuntimeStart && DuplicateRuntimeStart.error().Code == SceneRuntimeErrorCode::AlreadyRunning);
		const auto CoordinatedNegativeDelta = CoordinatedRuntime.Advance(CoordinatedScene, Timestep(-0.1));
		PF_CHECK(Tests, !CoordinatedNegativeDelta && CoordinatedNegativeDelta.error().Code == SceneRuntimeErrorCode::InvalidDeltaTime);
		const auto CoordinatedUpdate = CoordinatedRuntime.Advance(CoordinatedScene, Timestep(0.25));
		PF_CHECK(Tests, CoordinatedUpdate && CoordinatedUpdate->PhysicsSteps == 2);
		const auto RuntimeDiagnosticsSnapshot = RuntimeDiagnostics.CaptureSnapshot();
		PF_CHECK(Tests, RuntimeDiagnosticsSnapshot.Events.size() == 3);
		const uint64_t FirstCaptureSessionId = RuntimeDiagnosticsSnapshot.SessionId;
		bool SawScriptAdvance = false;
		bool SawPhysicsAdvance = false;
		bool SawAudioAdvance = false;
		for (const DiagnosticEvent& Event : RuntimeDiagnosticsSnapshot.Events)
		{
			SawScriptAdvance |= Event.Owner == "ScriptRuntime" && Event.Name == "ScriptRuntime::Advance";
			SawPhysicsAdvance |= Event.Owner == "PhysicsSceneRuntime" && Event.Name == "PhysicsSceneRuntime::Advance";
			SawAudioAdvance |= Event.Owner == "AudioSceneRuntime" && Event.Name == "AudioSceneRuntime::Advance";
		}
		PF_CHECK(Tests, SawScriptAdvance && SawPhysicsAdvance && SawAudioAdvance);
		RuntimeDiagnostics.SetCaptureEnabled(false);
		const DiagnosticSnapshot FirstStoppedCapture = RuntimeDiagnostics.CaptureSnapshot();
		PF_CHECK(Tests, FirstStoppedCapture.CaptureEndTimestampNanoseconds != 0);
		PF_CHECK(Tests, CoordinatedRuntime.Advance(CoordinatedScene, Timestep(0.1)).has_value());
		PF_CHECK(Tests, RuntimeDiagnostics.CaptureSnapshot().Events.size() == RuntimeDiagnosticsSnapshot.Events.size());
		const auto GameplayAtCaptureRestart = CoordinatedScriptEntity->GetTransform();
		const auto PhysicsAtCaptureRestart = CoordinatedPhysicsEntity->GetTransform();
		PF_CHECK(Tests, GameplayAtCaptureRestart && std::abs(GameplayAtCaptureRestart->Translation.x - 0.35f) < 0.0001f);
		PF_CHECK(Tests, PhysicsAtCaptureRestart.has_value());
		RuntimeDiagnostics.SetCaptureEnabled(true);
		const DiagnosticSnapshot SecondCaptureStarted = RuntimeDiagnostics.CaptureSnapshot();
		PF_CHECK(Tests, SecondCaptureStarted.SessionId > FirstCaptureSessionId);
		PF_CHECK(Tests, SecondCaptureStarted.Events.empty());
		PF_CHECK(Tests, SecondCaptureStarted.CaptureEndTimestampNanoseconds == 0);
		PF_CHECK(Tests, CoordinatedRuntime.IsRunning() && &CoordinatedRuntime == RunningRuntimeIdentity);
		PF_CHECK(Tests, CoordinatedRuntime.Advance(CoordinatedScene, Timestep(0.1)).has_value());
		const DiagnosticSnapshot SecondCaptureEvents = RuntimeDiagnostics.CaptureSnapshot();
		PF_CHECK(Tests, SecondCaptureEvents.Events.size() == 3);
		for (const DiagnosticEvent& Event : SecondCaptureEvents.Events)
			PF_CHECK(Tests, Event.SessionId == SecondCaptureStarted.SessionId);
		const auto GameplayAfterCaptureRestart = CoordinatedScriptEntity->GetTransform();
		const auto PhysicsAfterCaptureRestart = CoordinatedPhysicsEntity->GetTransform();
		PF_CHECK(Tests, GameplayAfterCaptureRestart && std::abs(GameplayAfterCaptureRestart->Translation.x - 0.45f) < 0.0001f);
		PF_CHECK(Tests, PhysicsAtCaptureRestart && PhysicsAfterCaptureRestart &&
			PhysicsAfterCaptureRestart->Translation.y < PhysicsAtCaptureRestart->Translation.y);
		PF_CHECK(Tests, CoordinatedRuntime.IsRunning() && &CoordinatedRuntime == RunningRuntimeIdentity);
		const auto CoordinatedTransform = CoordinatedScriptEntity->GetTransform();
		const auto PhysicsTransform = CoordinatedPhysicsEntity->GetTransform();
		PF_CHECK(Tests, CoordinatedTransform && std::abs(CoordinatedTransform->Translation.x - 0.45f) < 0.0001f);
		PF_CHECK(Tests, PhysicsTransform && PhysicsTransform->Translation.y < 0.0f);
		Scene OtherRuntimeScene;
		const auto WrongRuntimeScene = CoordinatedRuntime.Advance(OtherRuntimeScene, Timestep(0.1));
		PF_CHECK(Tests, !WrongRuntimeScene && WrongRuntimeScene.error().Code == SceneRuntimeErrorCode::DifferentScene);
		CoordinatedRuntime.Stop();
		PF_CHECK(Tests, !CoordinatedRuntime.IsRunning());
		const auto CoordinatedStoppedTransform = CoordinatedScriptEntity->GetTransform();
		PF_CHECK(Tests, CoordinatedStoppedTransform && CoordinatedStoppedTransform->Translation.x == -1.0f);

		const auto PhysicsPoseScriptAsset =
			CreateScriptAsset("physics_pose.lua", "function OnCreate() assert(entity:set_translation(4, 10, 0)) end\n"
												  "function OnUpdate() assert(entity:set_translation(6, 10, 0)) end\n");
		PF_CHECK(Tests, PhysicsPoseScriptAsset.has_value());
		if (!PhysicsPoseScriptAsset) return;
		Scene PhysicsPoseScene;
		const auto PhysicsPoseEntity = PhysicsPoseScene.CreateEntity("Script-teleported body");
		PF_CHECK(Tests, PhysicsPoseEntity.has_value());
		if (!PhysicsPoseEntity) return;
		PF_CHECK(Tests, PhysicsPoseEntity->SetRigidbody(RigidbodyComponent{}).has_value());
		PF_CHECK(Tests, PhysicsPoseEntity->SetBoxCollider(BoxColliderComponent{}).has_value());
		PF_CHECK(Tests, PhysicsPoseEntity->SetScript(ScriptComponent{ PhysicsPoseScriptAsset->ID }).has_value());
		SceneRuntime PhysicsPoseRuntime(*ProjectResult, RuntimeDescription);
		PF_CHECK(Tests, PhysicsPoseRuntime.Start(PhysicsPoseScene).has_value());
		const auto OnCreatePose = PhysicsPoseEntity->GetTransform();
		PF_CHECK(Tests, OnCreatePose && std::abs(OnCreatePose->Translation.x - 4.0f) < 0.0001f &&
							std::abs(OnCreatePose->Translation.y - 10.0f) < 0.0001f);
		const auto PhysicsPoseFrame = PhysicsPoseRuntime.Advance(PhysicsPoseScene, Timestep(0.1));
		const auto OnUpdatePose = PhysicsPoseEntity->GetTransform();
		PF_CHECK(Tests, PhysicsPoseFrame && PhysicsPoseFrame->PhysicsSteps == 1);
		PF_CHECK(Tests, OnUpdatePose && std::abs(OnUpdatePose->Translation.x - 6.0f) < 0.0001f && OnUpdatePose->Translation.y > 9.8f &&
							OnUpdatePose->Translation.y < 10.0f);
		PhysicsPoseRuntime.Stop();

		constexpr std::string_view ForceScript = R"(
function OnUpdate()
  local applied, message = entity:apply_force(0, 100, 0)
  assert(applied, message)
end
)";
		const auto ForceScriptAsset = CreateScriptAsset("force.lua", ForceScript);
		PF_CHECK(Tests, ForceScriptAsset.has_value());
		if (!ForceScriptAsset) return;
		Scene ScriptedPhysicsScene;
		const auto ScriptedBody = ScriptedPhysicsScene.CreateEntity("Script-driven body");
		PF_CHECK(Tests, ScriptedBody.has_value());
		if (!ScriptedBody) return;
		PF_CHECK(Tests, ScriptedBody->SetRigidbody(RigidbodyComponent{}).has_value());
		PF_CHECK(Tests, ScriptedBody->SetBoxCollider(BoxColliderComponent{}).has_value());
		PF_CHECK(Tests, ScriptedBody->SetScript(ScriptComponent{ ForceScriptAsset->ID }).has_value());
		SceneRuntime ScriptedPhysicsRuntime(*ProjectResult, RuntimeDescription);
		PF_CHECK(Tests, ScriptedPhysicsRuntime.Start(ScriptedPhysicsScene).has_value());
		const auto ScriptedPhysicsFrame = ScriptedPhysicsRuntime.Advance(ScriptedPhysicsScene, Timestep(0.1));
		const auto ScriptedBodyTransform = ScriptedBody->GetTransform();
		PF_CHECK(Tests, ScriptedPhysicsFrame && ScriptedPhysicsFrame->PhysicsSteps == 1);
		PF_CHECK(Tests, ScriptedBodyTransform && ScriptedBodyTransform->Translation.y > 0.0f);
		ScriptedPhysicsRuntime.Stop();

		const UUID ReusedServiceEntityID{ 0x5500000000000000ull, 0x51 };
		const auto StaleServiceScriptAsset = CreateScriptAsset("stale_services.lua", R"(
function OnDestroy()
  local applied, forceError = entity:apply_force(0, 100, 0)
  local played, playError = entity:play_audio()
  local paused, pauseError = entity:pause_audio()
  local resumed, resumeError = entity:resume_audio()
  local stopped, stopError = entity:stop_audio()
  assert(applied == nil and string.find(forceError, "no longer valid", 1, true) ~= nil)
  assert(played == nil and string.find(playError, "no longer valid", 1, true) ~= nil)
  assert(paused == nil and string.find(pauseError, "no longer valid", 1, true) ~= nil)
  assert(resumed == nil and string.find(resumeError, "no longer valid", 1, true) ~= nil)
  assert(stopped == nil and string.find(stopError, "no longer valid", 1, true) ~= nil)
end
)");
		const auto FreshServiceScriptAsset = CreateScriptAsset("fresh_services.lua", R"(
function OnUpdate()
  assert(entity:apply_force(0, 100, 0))
  assert(entity:play_audio())
end
function OnDestroy()
  assert(entity:apply_force(0, 1, 0))
  assert(entity:play_audio())
end
)");
		PF_CHECK(Tests, StaleServiceScriptAsset && FreshServiceScriptAsset);
		if (!StaleServiceScriptAsset || !FreshServiceScriptAsset) return;

		const std::vector<std::byte> ServiceAudioWave = MakeSilentScriptTestWave();
		const auto ServiceAudioAsset =
			AssetOperations::CreateAssetFromBytes(ProjectResult->GetAssetRegistry(), ProjectResult->GetRootPath(),
				std::as_bytes(std::span(ServiceAudioWave.data(), ServiceAudioWave.size())), "Assets/Scripts/service_test.wav");
		PF_CHECK(Tests, ServiceAudioAsset.has_value());
		if (!ServiceAudioAsset) return;

		auto ServiceAudioEngineResult = AudioEngine::Create({ AudioOutputBackend::Null });
		PF_CHECK(Tests, ServiceAudioEngineResult.has_value());
		if (!ServiceAudioEngineResult) return;
		AudioEngine ServiceAudioEngine = std::move(*ServiceAudioEngineResult);
		AudioAssetCache ServiceAudioCache(ServiceAudioEngine, ProjectResult->GetRootPath(), ProjectResult->GetAssetRegistry());
		AudioSceneRuntime ServiceAudioRuntime(ServiceAudioEngine, ServiceAudioCache);
		PhysicsSceneRuntime ServicePhysicsRuntime(RuntimeDescription.Physics);
		Scene ReusedServiceScene;
		const auto OriginalServiceEntity = ReusedServiceScene.CreateEntityWithUUID(ReusedServiceEntityID, "Original service entity");
		PF_CHECK(Tests, OriginalServiceEntity.has_value());
		if (!OriginalServiceEntity) return;
		PF_CHECK(Tests, OriginalServiceEntity->SetRigidbody(RigidbodyComponent{}).has_value());
		PF_CHECK(Tests, OriginalServiceEntity->SetBoxCollider(BoxColliderComponent{}).has_value());
		PF_CHECK(Tests,
			OriginalServiceEntity->SetAudioSource(AudioSourceComponent{ ServiceAudioAsset->ID, 1.0f, false, false, true }).has_value());
		PF_CHECK(Tests, OriginalServiceEntity->SetScript(ScriptComponent{ StaleServiceScriptAsset->ID }).has_value());
		PF_CHECK(Tests, ServicePhysicsRuntime.Start(ReusedServiceScene).has_value());
		PF_CHECK(Tests, ServiceAudioRuntime.Start(ReusedServiceScene).has_value());
		PF_CHECK(Tests, ServiceAudioRuntime.GetPlaybackCount() == 0);
		ScriptRuntimeServices ReusedServiceBindings{ .Physics = &ServicePhysicsRuntime, .Audio = &ServiceAudioRuntime };
		ScriptRuntime ReusedServiceScriptRuntime(*ProjectResult, {}, ReusedServiceBindings);
		PF_CHECK(Tests, ReusedServiceScriptRuntime.Start(ReusedServiceScene).has_value());
		PF_CHECK(Tests, ReusedServiceScriptRuntime.Advance(ReusedServiceScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, ReusedServiceScene.DestroyEntity(*OriginalServiceEntity).has_value());
		const auto ReplacementServiceEntity = ReusedServiceScene.CreateEntityWithUUID(ReusedServiceEntityID, "Replacement service entity");
		PF_CHECK(Tests, ReplacementServiceEntity.has_value());
		if (!ReplacementServiceEntity) return;
		PF_CHECK(Tests, ReplacementServiceEntity->SetRigidbody(RigidbodyComponent{}).has_value());
		PF_CHECK(Tests, ReplacementServiceEntity->SetBoxCollider(BoxColliderComponent{}).has_value());
		PF_CHECK(Tests,
			ReplacementServiceEntity->SetAudioSource(AudioSourceComponent{ ServiceAudioAsset->ID, 1.0f, false, false, false }).has_value());
		PF_CHECK(Tests, ReusedServiceScriptRuntime.Advance(ReusedServiceScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, ReusedServiceScriptRuntime.GetDiagnostics().empty());
		PF_CHECK(Tests, ServiceAudioRuntime.GetPlaybackCount() == 0);
		const auto NoStaleForceFrame = ServicePhysicsRuntime.Advance(ReusedServiceScene, Timestep(0.1));
		const auto NoStaleForceTransform = ReplacementServiceEntity->GetTransform();
		PF_CHECK(Tests, NoStaleForceFrame && *NoStaleForceFrame == 1);
		PF_CHECK(Tests, NoStaleForceTransform && NoStaleForceTransform->Translation.y < 0.0f);
		PF_CHECK(Tests, ServiceAudioRuntime.Advance(ReusedServiceScene).has_value());
		PF_CHECK(Tests, ServiceAudioRuntime.GetPlaybackCount() == 0);

		PF_CHECK(Tests, ReplacementServiceEntity->SetScript(ScriptComponent{ FreshServiceScriptAsset->ID }).has_value());
		PF_CHECK(Tests, ReusedServiceScriptRuntime.Advance(ReusedServiceScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, ReusedServiceScriptRuntime.GetDiagnostics().empty());
		PF_CHECK(Tests, ServiceAudioRuntime.GetPlaybackCount() == 1);
		const auto FreshServiceForceFrame = ServicePhysicsRuntime.Advance(ReusedServiceScene, Timestep(0.1));
		const auto FreshServiceTransform = ReplacementServiceEntity->GetTransform();
		PF_CHECK(Tests, FreshServiceForceFrame && *FreshServiceForceFrame == 1);
		PF_CHECK(Tests,
			FreshServiceTransform && NoStaleForceTransform && FreshServiceTransform->Translation.y > NoStaleForceTransform->Translation.y);
		ReusedServiceScriptRuntime.Stop();
		PF_CHECK(Tests, ReusedServiceScriptRuntime.GetDiagnostics().empty());
		ServiceAudioRuntime.Stop();
		ServicePhysicsRuntime.Stop();

		constexpr std::string_view UnavailableServicesScript = R"(
function OnUpdate()
  local applied, forceMessage = entity:apply_force(0, 1, 0)
  assert(applied == nil and string.find(forceMessage, "unavailable", 1, true) ~= nil)
  local played, audioMessage = entity:play_audio()
  assert(played == nil and string.find(audioMessage, "unavailable", 1, true) ~= nil)
  local pressed, inputMessage = input.is_key_pressed(87)
  assert(pressed == nil and string.find(inputMessage, "unavailable", 1, true) ~= nil)
end
)";
		const auto UnavailableServicesAsset = CreateScriptAsset("services_unavailable.lua", UnavailableServicesScript);
		PF_CHECK(Tests, UnavailableServicesAsset.has_value());
		if (!UnavailableServicesAsset) return;
		Scene UnavailableServicesScene;
		const auto UnavailableServicesEntity = UnavailableServicesScene.CreateEntity("No runtime services");
		PF_CHECK(Tests, UnavailableServicesEntity.has_value());
		if (!UnavailableServicesEntity) return;
		PF_CHECK(Tests, UnavailableServicesEntity->SetScript(ScriptComponent{ UnavailableServicesAsset->ID }).has_value());
		ScriptRuntime UnavailableServicesRuntime(*ProjectResult);
		PF_CHECK(Tests, UnavailableServicesRuntime.Start(UnavailableServicesScene).has_value());
		PF_CHECK(Tests, UnavailableServicesRuntime.Advance(UnavailableServicesScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, UnavailableServicesRuntime.GetDiagnostics().empty());
		UnavailableServicesRuntime.Stop();

		constexpr std::string_view InputScript = R"(
local Updated = false
function OnCreate()
  local invalidKey, keyMessage = input.is_key_pressed("W")
  assert(invalidKey == nil and keyMessage ~= nil)
  local invalidButton, buttonMessage = input.is_mouse_button_pressed("left")
  assert(invalidButton == nil and buttonMessage ~= nil)
end
function OnUpdate()
  local x, y = input.get_mouse_position()
  if not Updated then
    assert(input.is_key_pressed(87))
    assert(not input.is_key_pressed(83))
    assert(input.is_mouse_button_pressed(0))
    assert(not input.is_mouse_button_pressed(1))
    assert(x == 12.5 and y == 9.25)
    Updated = true
  else
    assert(not input.is_key_pressed(87))
    assert(not input.is_mouse_button_pressed(0))
    assert(x == 14.5 and y == 7.25)
  end
  assert(entity:set_translation(x, y, 0))
end
)";
		const auto InputScriptAsset = CreateScriptAsset("input.lua", InputScript);
		PF_CHECK(Tests, InputScriptAsset.has_value());
		if (!InputScriptAsset) return;

		Input TestInput;
		KeyPressedEvent WPressed(87, 0);
		MouseButtonPressedEvent LeftPressed(0);
		MouseMovedEvent InitialMousePosition(12.5f, 9.25f);
		TestInput.OnEvent(WPressed);
		TestInput.OnEvent(LeftPressed);
		TestInput.OnEvent(InitialMousePosition);

		Scene InputTestScene;
		const auto InputTestEntity = InputTestScene.CreateEntity("Input-driven script");
		PF_CHECK(Tests, InputTestEntity && InputTestEntity->SetScript(ScriptComponent{ InputScriptAsset->ID }));
		if (!InputTestEntity) return;
		ScriptRuntimeServices InputServices;
		InputServices.InputState = &TestInput;
		ScriptRuntime InputRuntime(*ProjectResult, {}, InputServices);
		PF_CHECK(Tests, InputRuntime.Start(InputTestScene).has_value());
		PF_CHECK(Tests, InputRuntime.Advance(InputTestScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, InputRuntime.GetDiagnostics().empty());
		KeyReleasedEvent WReleased(87);
		MouseButtonReleasedEvent LeftReleased(0);
		MouseMovedEvent UpdatedMousePosition(14.5f, 7.25f);
		TestInput.OnEvent(WReleased);
		TestInput.OnEvent(LeftReleased);
		TestInput.OnEvent(UpdatedMousePosition);
		PF_CHECK(Tests, InputRuntime.Advance(InputTestScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, InputRuntime.GetDiagnostics().empty());
		const auto InputUpdatedTransform = InputTestEntity->GetTransform();
		PF_CHECK(Tests, InputUpdatedTransform && std::abs(InputUpdatedTransform->Translation.x - 14.5f) < 0.0001f &&
							std::abs(InputUpdatedTransform->Translation.y - 7.25f) < 0.0001f);
		InputRuntime.Stop();

		const auto ActionInputScriptAsset = CreateScriptAsset("action_input.lua", R"(
local updates = 0
function OnUpdate()
  updates = updates + 1
  if updates == 1 then
    assert(input.was_action_pressed("Jump"))
    assert(input.is_action_held("Jump"))
    local x, y = input.get_vector("Move")
    assert(x == 1 and y == 0)
    assert(input.get_axis("Throttle") == 1)
    local axis, axisMessage = input.get_axis("Jump")
    assert(axis == nil and axisMessage ~= nil)
    local unknown, unknownMessage = input.is_action_held("Missing")
    assert(unknown == nil and unknownMessage ~= nil)
  else
    assert(not input.is_action_held("Jump"))
    assert(input.was_action_released("Jump"))
  end
end
)" );
		PF_CHECK(Tests, ActionInputScriptAsset.has_value());
		if (!ActionInputScriptAsset) return;
		const auto ActionInputObserverAsset = CreateScriptAsset("action_input_observer.lua", R"(
local updates = 0
function OnUpdate()
  updates = updates + 1
  if updates == 1 then
    assert(input.was_action_pressed("Jump"))
    assert(input.is_action_held("Jump"))
  else
    assert(input.was_action_released("Jump"))
    assert(not input.is_action_held("Jump"))
  end
end
)" );
		PF_CHECK(Tests, ActionInputObserverAsset.has_value());
		if (!ActionInputObserverAsset) return;

		GameplayInputService GameplayInput;
		GameplayActionMap ActionMap;
		GameplayActionDefinition JumpAction;
		JumpAction.Id = "Jump";
		JumpAction.Type = ActionValueType::Digital;
		ActionBinding JumpBinding;
		JumpBinding.Id = "Jump.primary";
		JumpBinding.Primary = { InputDevice::Keyboard, static_cast<uint16_t>(InputKey::Space) };
		JumpAction.Bindings.push_back(JumpBinding);
		ActionMap.Actions.push_back(JumpAction);
		GameplayActionDefinition MoveAction;
		MoveAction.Id = "Move";
		MoveAction.Type = ActionValueType::Axis2D;
		ActionBinding MoveRight;
		MoveRight.Id = "Move.right";
		MoveRight.Primary = { InputDevice::Keyboard, static_cast<uint16_t>(InputKey::D) };
		MoveRight.ScaleX = 1.0f;
		ActionBinding MoveUp;
		MoveUp.Id = "Move.up";
		MoveUp.Primary = { InputDevice::Keyboard, static_cast<uint16_t>(InputKey::W) };
		MoveUp.ScaleY = 1.0f;
		MoveAction.Bindings = { MoveRight, MoveUp };
		ActionMap.Actions.push_back(MoveAction);
		GameplayActionDefinition ThrottleAction;
		ThrottleAction.Id = "Throttle";
		ThrottleAction.Type = ActionValueType::Axis1D;
		ActionBinding ThrottleBinding;
		ThrottleBinding.Id = "Throttle.key";
		ThrottleBinding.Kind = ActionBindingKind::Axis;
		ThrottleBinding.Primary = { InputDevice::Keyboard, static_cast<uint16_t>(InputKey::E) };
		ThrottleAction.Bindings.push_back(ThrottleBinding);
		ActionMap.Actions.push_back(ThrottleAction);
		PF_CHECK(Tests, GameplayInput.SetActionMap(std::move(ActionMap)).has_value());
		InputRoutingState ActionRouting;
		ActionRouting.FocusedWindow = 17;
		ActionRouting.Contexts[0] = { .Context = InputContext::Gameplay,
			.Consumes = InputDeviceMask::None, .OwnerWindow = 17, .Active = true };
		ActionRouting.ContextCount = 1;
		PF_CHECK(Tests, GameplayInput.QueueEvent({ .Kind = InputEventKind::Pressed,
			.Control = { InputDevice::Keyboard, static_cast<uint16_t>(InputKey::Space) }, .Window = 17 }));
		PF_CHECK(Tests, GameplayInput.QueueEvent({ .Kind = InputEventKind::Pressed,
			.Control = { InputDevice::Keyboard, static_cast<uint16_t>(InputKey::D) }, .Window = 17 }));
		PF_CHECK(Tests, GameplayInput.QueueEvent({ .Kind = InputEventKind::Pressed,
			.Control = { InputDevice::Keyboard, static_cast<uint16_t>(InputKey::E) }, .Window = 17 }));
		GameplayInput.PublishFrame(ActionRouting);
		PF_CHECK(Tests, GameplayInput.FindAction("Jump") && GameplayInput.FindAction("Jump")->Held &&
			GameplayInput.FindAction("Jump")->Pressed);
		PF_CHECK(Tests, GameplayInput.FindAction("Move") && GameplayInput.FindAction("Move")->X == 1.0f);
		PF_CHECK(Tests, GameplayInput.FindAction("Throttle") && GameplayInput.FindAction("Throttle")->X == 1.0f);

		Scene ActionInputScene;
		const auto ActionInputEntity = ActionInputScene.CreateEntity("Action input consumer");
		const auto ActionInputObserver = ActionInputScene.CreateEntity("Shared snapshot observer");
		PF_CHECK(Tests, ActionInputEntity && ActionInputObserver);
		if (!ActionInputEntity || !ActionInputObserver) return;
		PF_CHECK(Tests, ActionInputEntity->SetScript(ScriptComponent{ ActionInputScriptAsset->ID }).has_value());
		PF_CHECK(Tests, ActionInputObserver->SetScript(ScriptComponent{ ActionInputObserverAsset->ID }).has_value());
		ScriptRuntimeServices ActionInputServices;
		ActionInputServices.GameplayInput = &GameplayInput;
		ScriptRuntime ActionInputRuntime(*ProjectResult, {}, ActionInputServices);
		PF_CHECK(Tests, ActionInputRuntime.Start(ActionInputScene).has_value());
		PF_CHECK(Tests, ActionInputRuntime.Advance(ActionInputScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, ActionInputRuntime.GetDiagnostics().empty());
		PF_CHECK(Tests, GameplayInput.QueueEvent({ .Kind = InputEventKind::Released,
			.Control = { InputDevice::Keyboard, static_cast<uint16_t>(InputKey::Space) }, .Window = 17 }));
		GameplayInput.PublishFrame(ActionRouting);
		PF_CHECK(Tests, ActionInputRuntime.Advance(ActionInputScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, ActionInputRuntime.GetDiagnostics().empty());
		ActionInputRuntime.Stop();

		KeyPressedEvent CoordinatorWPressed(87, 0);
		MouseButtonPressedEvent CoordinatorLeftPressed(0);
		MouseMovedEvent CoordinatorMousePosition(12.5f, 9.25f);
		TestInput.OnEvent(CoordinatorWPressed);
		TestInput.OnEvent(CoordinatorLeftPressed);
		TestInput.OnEvent(CoordinatorMousePosition);
		Scene CoordinatedInputScene;
		const auto CoordinatedInputEntity = CoordinatedInputScene.CreateEntity("Coordinated input script");
		PF_CHECK(Tests, CoordinatedInputEntity && CoordinatedInputEntity->SetScript(ScriptComponent{ InputScriptAsset->ID }));
		if (!CoordinatedInputEntity) return;
		SceneRuntimeServices HostServices{ .InputState = &TestInput };
		SceneRuntime CoordinatedInputRuntime(*ProjectResult, RuntimeDescription, HostServices);
		PF_CHECK(Tests, CoordinatedInputRuntime.Start(CoordinatedInputScene).has_value());
		PF_CHECK(Tests, CoordinatedInputRuntime.Advance(CoordinatedInputScene, Timestep(1.0 / 60.0)).has_value());
		const auto CoordinatedInputTransform = CoordinatedInputEntity->GetTransform();
		PF_CHECK(Tests, CoordinatedInputTransform && std::abs(CoordinatedInputTransform->Translation.x - 12.5f) < 0.0001f &&
							std::abs(CoordinatedInputTransform->Translation.y - 9.25f) < 0.0001f);
		CoordinatedInputRuntime.Stop();

		Scene RollbackScene;
		const auto RollbackScriptEntity = RollbackScene.CreateEntity("Rollback script");
		const auto RollbackPhysicsEntity = RollbackScene.CreateEntity("Rollback physics entity");
		PF_CHECK(Tests, RollbackScriptEntity && RollbackPhysicsEntity);
		if (!RollbackScriptEntity || !RollbackPhysicsEntity) return;
		PF_CHECK(Tests, RollbackScriptEntity->SetScript(ScriptComponent{ TransformScriptAsset->ID }).has_value());
		PF_CHECK(Tests, RollbackPhysicsEntity->SetRigidbody(RigidbodyComponent{}).has_value());
		PF_CHECK(Tests, RollbackPhysicsEntity->SetBoxCollider(BoxColliderComponent{}).has_value());
		SceneRuntimeDesc InvalidScriptDescription = RuntimeDescription;
		InvalidScriptDescription.Scripting.MaxSourceBytes = 0;
		SceneRuntime RollbackRuntime(*ProjectResult, InvalidScriptDescription);
		const auto FailedRuntimeStart = RollbackRuntime.Start(RollbackScene);
		PF_CHECK(Tests, !FailedRuntimeStart && FailedRuntimeStart.error().Subsystem == SceneRuntimeSubsystem::Scripting);
		PF_CHECK(Tests, !RollbackRuntime.IsRunning());
		const auto RolledBackTransform = RollbackScriptEntity->GetTransform();
		PF_CHECK(Tests, RolledBackTransform && RolledBackTransform->Translation.x == 0.0f);

		constexpr std::string_view SyntaxErrorScript = "function OnUpdate(\n";
		const auto SyntaxErrorAsset = CreateScriptAsset("syntax_error.lua", SyntaxErrorScript);
		PF_CHECK(Tests, SyntaxErrorAsset.has_value());
		if (!SyntaxErrorAsset) return;
		Scene CompileErrorScene;
		const auto CompileErrorEntity = CompileErrorScene.CreateEntity("Compile error");
		PF_CHECK(Tests, CompileErrorEntity && CompileErrorEntity->SetScript(ScriptComponent{ SyntaxErrorAsset->ID }));
		ScriptRuntime CompileErrorRuntime(*ProjectResult);
		PF_CHECK(Tests, CompileErrorRuntime.Start(CompileErrorScene).has_value());
		PF_CHECK(Tests, CompileErrorRuntime.GetScriptCount() == 0 && CompileErrorRuntime.GetDiagnostics().size() == 1);
		PF_CHECK(Tests, CompileErrorRuntime.GetDiagnostics().front().Code == ScriptDiagnosticCode::CompileFailed);
		CompileErrorRuntime.Stop();
		PF_CHECK(Tests, CompileErrorRuntime.GetDiagnostics().size() == 1);
		CompileErrorRuntime.ClearDiagnostics();
		PF_CHECK(Tests, CompileErrorRuntime.GetDiagnostics().empty());

		constexpr std::string_view InfiniteScript = "function OnUpdate() while true do end end\n";
		const auto InfiniteScriptAsset = CreateScriptAsset("instruction_limit.lua", InfiniteScript);
		PF_CHECK(Tests, InfiniteScriptAsset.has_value());
		if (!InfiniteScriptAsset) return;
		Scene InfiniteScene;
		const auto InfiniteEntity = InfiniteScene.CreateEntity("Instruction-limited script");
		PF_CHECK(Tests, InfiniteEntity && InfiniteEntity->SetScript(ScriptComponent{ InfiniteScriptAsset->ID }));
		ScriptRuntimeDesc LimitedDescription;
		LimitedDescription.MaxInstructionsPerCallback = 1000;
		ScriptRuntime LimitedRuntime(*ProjectResult, LimitedDescription);
		PF_CHECK(Tests, LimitedRuntime.Start(InfiniteScene).has_value());
		PF_CHECK(Tests, LimitedRuntime.Advance(InfiniteScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, LimitedRuntime.GetDiagnostics().size() == 1);
		PF_CHECK(Tests, LimitedRuntime.GetDiagnostics().front().Code == ScriptDiagnosticCode::UpdateFailed);
		PF_CHECK(Tests, LimitedRuntime.GetDiagnostics().front().Message.find("instruction budget") != std::string::npos);
		PF_CHECK(Tests, LimitedRuntime.Advance(InfiniteScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, LimitedRuntime.GetDiagnostics().size() == 1);
		LimitedRuntime.Stop();

		constexpr std::string_view MemoryLimitScript = R"(
function OnCreate()
  local values = {}
  while true do
    values[#values + 1] = string.rep("x", 4096)
  end
end
)";
		const auto MemoryScriptAsset = CreateScriptAsset("memory_limit.lua", MemoryLimitScript);
		PF_CHECK(Tests, MemoryScriptAsset.has_value());
		if (!MemoryScriptAsset) return;
		Scene MemoryScene;
		const auto MemoryEntity = MemoryScene.CreateEntity("Memory-limited script");
		PF_CHECK(Tests, MemoryEntity && MemoryEntity->SetScript(ScriptComponent{ MemoryScriptAsset->ID }));
		ScriptRuntimeDesc MemoryDescription;
		MemoryDescription.MaxLuaMemoryBytes = 512u * 1024u;
		MemoryDescription.MaxTotalLuaMemoryBytes = 2u * 1024u * 1024u;
		ScriptRuntime MemoryRuntime(*ProjectResult, MemoryDescription);
		PF_CHECK(Tests, MemoryRuntime.Start(MemoryScene).has_value());
		PF_CHECK(Tests, MemoryRuntime.GetScriptCount() == 0 && MemoryRuntime.GetDiagnostics().size() == 1);
		PF_CHECK(Tests, MemoryRuntime.GetDiagnostics().front().Code == ScriptDiagnosticCode::InitializationFailed);
		MemoryRuntime.Stop();
	}
} // namespace

void RunScriptRuntimeTests(TestRunner& Tests)
{
	TestScriptRuntime(Tests);
}
