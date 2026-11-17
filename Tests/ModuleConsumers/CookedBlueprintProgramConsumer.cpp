#include "Assets/CookedAssetCatalog.h"
#include "Runtime/CookedContentProvider.h"
#include "Runtime/PlayerSession.h"
#include "Scene/Components/BoxColliderComponent.h"
#include "Scene/Components/RigidbodyComponent.h"
#include "Scene/Components/TransformComponent.h"
#include "Scene/SceneSerializer.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <vector>

namespace
{
	int Fail(int Code, const char* Stage)
	{
		std::cerr << "Cooked Blueprint runtime consumer failed at " << Stage << " (code " << Code << ")\n";
		return Code;
	}

	const PulseForge::VerifiedBlueprintProgram* FindEventProgram(
		const PulseForge::VerifiedBlueprintAsset& Asset, PulseForge::BlueprintEventKind Event)
	{
		for (const PulseForge::BlueprintVerifiedEventProgram& Program : Asset.GetEvents())
			if (Program.Event == Event)
				return &Program.Program;
		return nullptr;
	}

	PulseForge::CookedAssetCompatibility GetCompatibility()
	{
		using namespace PulseForge;
#if defined(_WIN32)
		return { "Windows", "Vulkan", std::string(CookedRuntimeABIIdentifier) };
#elif defined(__APPLE__)
		return { "macOS", "Vulkan", std::string(CookedRuntimeABIIdentifier) };
#else
		return { "Linux", "Vulkan", std::string(CookedRuntimeABIIdentifier) };
#endif
	}

	bool WritePayload(const std::filesystem::path& Root, const PulseForge::CookedAssetEntry& Entry,
		std::span<const std::byte> Payload)
	{
		std::error_code Error;
		const std::filesystem::path Path = Root / std::filesystem::u8path(Entry.RelativePayloadPath);
		std::filesystem::create_directories(Path.parent_path(), Error);
		if (Error) return false;
		std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
		if (!Output) return false;
		Output.write(reinterpret_cast<const char*>(Payload.data()), static_cast<std::streamsize>(Payload.size()));
		return static_cast<bool>(Output);
	}

	PulseForge::CookedAssetEntry MakeEntry(PulseForge::AssetID Asset,
		PulseForge::CookedAssetProductKind Product, uint32_t Version, std::string Path,
		std::span<const std::byte> Payload)
	{
		using namespace PulseForge;
		const CookedAssetDigest Digest = CookedAssetCatalog::ComputePayloadDigest(Payload);
		return { Asset, Product, {}, std::move(Path), Version, Payload.size(), Digest.Algorithm, Digest.Value, {}, {} };
	}

	std::vector<std::byte> MakeSilentWave()
	{
		std::vector<std::byte> Data(46);
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
		Write32(40, 2);
		return Data;
	}
}

int main()
{
	using namespace PulseForge;
	const auto Unique = UUID::Generate();
	if (!Unique) return Fail(1, "generate fixture identity");
	std::error_code Error;
	const std::filesystem::path SystemTemporaryRoot = std::filesystem::temp_directory_path(Error);
	if (Error) return Fail(2, "resolve system temporary directory");
	const std::filesystem::path TemporaryRoot = std::filesystem::weakly_canonical(SystemTemporaryRoot, Error);
	if (Error) return Fail(2, "canonicalize system temporary directory");
	const std::filesystem::path Root = TemporaryRoot / ("PF-CookedBlueprintRuntime-" + Unique->ToString());
	if (Error || !std::filesystem::create_directories(Root, Error) || Error)
		return Fail(2, "create temporary cooked package");
	struct Cleanup final
	{
		std::filesystem::path Path;
		~Cleanup() { std::error_code Ignored; std::filesystem::remove_all(Path, Ignored); }
	} CleanupRoot{ Root };

	const AssetID BlueprintID{ 0x7000000000004000ull, 0x8000000000000001ull };
	const AssetID SceneID{ 0x7000000000004000ull, 0x8000000000000002ull };
	const UUID EntityID{ 0x7000000000004000ull, 0x8000000000000003ull };
	const AssetID TransitionBlueprintID{ 0x7000000000004000ull, 0x8000000000000004ull };
	const AssetID DestinationSceneID{ 0x7000000000004000ull, 0x8000000000000005ull };
	const UUID TransitionEntityID{ 0x7000000000004000ull, 0x8000000000000006ull };
	const AssetID AudioID{ 0x7000000000004000ull, 0x8000000000000007ull };
	BlueprintProgramSource StartTimer;
	StartTimer.Version = BlueprintProgramFormatVersion;
	StartTimer.Constants = { 0.05f };
	StartTimer.ScalarPropertyIds = { "health", "raycast-hit" };
	StartTimer.TimerNames = { "cooldown" };
	StartTimer.Instructions = {
		{ BlueprintOpcode::RequestTimerSelf, 0, 0, UUID(71, 1) },
		{ BlueprintOpcode::Return, 0, 0, UUID(71, 2) }
	};
	BlueprintProgramSource TimerCallback;
	TimerCallback.Version = BlueprintProgramFormatVersion;
	TimerCallback.Constants = { 7.0f };
	TimerCallback.ScalarPropertyIds = { "health", "raycast-hit" };
	TimerCallback.TimerNames = { "cooldown" };
	TimerCallback.Instructions = {
		{ BlueprintOpcode::JumpIfTimer, 0, 2, UUID(72, 1) },
		{ BlueprintOpcode::Return, 0, 0, UUID(72, 2) },
		{ BlueprintOpcode::PushConstant, 0, 0, UUID(72, 3) },
		{ BlueprintOpcode::WriteScalarProperty, 0, 0, UUID(72, 4) },
		{ BlueprintOpcode::Return, 0, 0, UUID(72, 5) }
	};
	const std::array<BlueprintEventProgramSource, 2> BlueprintEvents{
		BlueprintEventProgramSource{ BlueprintEventKind::OnCreate, StartTimer },
		BlueprintEventProgramSource{ BlueprintEventKind::OnTimer, TimerCallback }
	};
	BlueprintProgramSource SpawnOnUpdate;
	SpawnOnUpdate.Version = BlueprintProgramFormatVersion;
	SpawnOnUpdate.Constants = { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 20.0f, 8.0f, 9.0f, 10.0f };
	SpawnOnUpdate.ScalarPropertyIds = { "health", "raycast-hit" };
	for (uint32_t Index = 0; Index < 7; ++Index)
		SpawnOnUpdate.Instructions.push_back({ BlueprintOpcode::PushConstant, Index, 0, UUID(74, Index + 1) });
	SpawnOnUpdate.Instructions.insert(SpawnOnUpdate.Instructions.end(), {
		{ BlueprintOpcode::Raycast, 0, 0, UUID(74, 8) },
		{ BlueprintOpcode::StoreScalar, 1, 0, UUID(74, 9) },
		{ BlueprintOpcode::LoadScalar, 1, 0, UUID(74, 10) },
		{ BlueprintOpcode::WriteScalarProperty, 1, 0, UUID(74, 11) },
		{ BlueprintOpcode::PushConstant, 7, 0, UUID(74, 1) },
		{ BlueprintOpcode::PushConstant, 8, 0, UUID(74, 2) },
		{ BlueprintOpcode::PushConstant, 9, 0, UUID(74, 3) },
		{ BlueprintOpcode::RequestSpawnEntity, 0, 0, UUID(74, 12) },
		{ BlueprintOpcode::Return, 0, 0, UUID(74, 13) }
	});
	const std::array<BlueprintEventProgramSource, 3> BlueprintEventsWithSpawn{
		BlueprintEvents[0], BlueprintEvents[1],
		BlueprintEventProgramSource{ BlueprintEventKind::OnUpdate, SpawnOnUpdate }
	};
	auto BlueprintPayload = SerializeBlueprintAsset(BlueprintEventsWithSpawn);
	if (!BlueprintPayload) return Fail(3, "compile and serialize Blueprint event product");
	BlueprintProgramSource RequestTransition;
	RequestTransition.Version = BlueprintProgramFormatVersion;
	RequestTransition.SceneTransitionAssets = { DestinationSceneID };
	RequestTransition.Instructions = {
		{ BlueprintOpcode::RequestSceneTransition, 0, 0, UUID(75, 1) },
		{ BlueprintOpcode::Return, 0, 0, UUID(75, 2) }
	};
	const std::array<BlueprintEventProgramSource, 1> TransitionEvents{
		BlueprintEventProgramSource{ BlueprintEventKind::OnUpdate, RequestTransition }
	};
	auto TransitionBlueprintPayload = SerializeBlueprintAsset(TransitionEvents);
	if (!TransitionBlueprintPayload) return Fail(3, "compile and serialize Blueprint transition product");

	Scene AuthoredScene;
	const auto AuthoredEntity = AuthoredScene.CreateEntityWithUUID(EntityID, "Cooked Blueprint timer");
	if (!AuthoredEntity || !AuthoredEntity->SetBlueprint(BlueprintComponent{ BlueprintID, true, {} }))
		return Fail(4, "author scene Blueprint reference");
	constexpr UUID RaycastTargetID{ 0x7000000000004000ull, 0x8000000000000008ull };
	const auto RaycastTarget = AuthoredScene.CreateEntityWithUUID(RaycastTargetID, "Cooked Blueprint query target");
	if (!RaycastTarget) return Fail(4, "author cooked Blueprint raycast target");
	TransformComponent RaycastTargetTransform;
	RaycastTargetTransform.Translation.x = 4.0f;
	if (!RaycastTarget->SetTransform(RaycastTargetTransform) ||
		!RaycastTarget->SetRigidbody({ RigidbodyMotionType::Static }) ||
		!RaycastTarget->SetCollider(BoxColliderComponent{}))
		return Fail(4, "author cooked Blueprint raycast collider");
	const auto TransitionEntity = AuthoredScene.CreateEntityWithUUID(TransitionEntityID, "Cooked Blueprint transition");
	if (!TransitionEntity || !TransitionEntity->SetBlueprint(BlueprintComponent{ TransitionBlueprintID, true, {} }))
		return Fail(4, "author scene transition Blueprint reference");
	const auto SceneText = SceneSerializer::Serialize(AuthoredScene);
	if (!SceneText) return Fail(5, "serialize fixture scene");
	const std::span<const std::byte> ScenePayload(reinterpret_cast<const std::byte*>(SceneText->data()), SceneText->size());
	Scene DestinationScene;
	if (!DestinationScene.CreateEntityWithUUID(UUID(75, 3), "Cooked destination marker"))
		return Fail(5, "author destination scene");
	const auto DestinationSceneText = SceneSerializer::Serialize(DestinationScene);
	if (!DestinationSceneText) return Fail(5, "serialize destination scene");
	const std::span<const std::byte> DestinationScenePayload(
		reinterpret_cast<const std::byte*>(DestinationSceneText->data()), DestinationSceneText->size());

	CookedAssetEntry BlueprintEntry = MakeEntry(BlueprintID, CookedAssetProductKind::BlueprintProgram,
		BlueprintAssetFormatVersion, "b/blueprint.pfbp", *BlueprintPayload);
	CookedAssetEntry TransitionBlueprintEntry = MakeEntry(TransitionBlueprintID, CookedAssetProductKind::BlueprintProgram,
		BlueprintAssetFormatVersion, "b/transition.pfbp", *TransitionBlueprintPayload);
	CookedAssetEntry SceneEntry = MakeEntry(SceneID, CookedAssetProductKind::Scene, 1, "s/startup.pfscene", ScenePayload);
	CookedAssetEntry DestinationSceneEntry = MakeEntry(
		DestinationSceneID, CookedAssetProductKind::Scene, 1, "s/destination.pfscene", DestinationScenePayload);
	const std::vector<std::byte> AudioPayload = MakeSilentWave();
	CookedAssetEntry AudioEntry = MakeEntry(AudioID, CookedAssetProductKind::Audio, 1, "a/silence.wav", AudioPayload);
	if (!WritePayload(Root, BlueprintEntry, *BlueprintPayload) ||
		!WritePayload(Root, TransitionBlueprintEntry, *TransitionBlueprintPayload) ||
		!WritePayload(Root, SceneEntry, ScenePayload) || !WritePayload(Root, DestinationSceneEntry, DestinationScenePayload) ||
		!WritePayload(Root, AudioEntry, AudioPayload))
		return Fail(6, "write cooked payloads");
	const CookedAssetCompatibility Compatibility = GetCompatibility();
	const auto Manifest = CookedAssetCatalog::SerializeManifest(
		UUID(73, 1), SceneID, Compatibility, {},
		{ BlueprintEntry, TransitionBlueprintEntry, SceneEntry, DestinationSceneEntry, AudioEntry });
	if (!Manifest) return Fail(7, "serialize cooked manifest");
	{
		std::ofstream Output(Root / CookedAssetCatalog::ManifestFileName, std::ios::binary | std::ios::trunc);
		Output.write(Manifest->data(), static_cast<std::streamsize>(Manifest->size()));
		if (!Output) return Fail(8, "write cooked manifest");
	}
	auto Provider = CookedContentProvider::Open(Root, Compatibility);
	if (!Provider) return Fail(9, "open cooked-only content provider");
	auto LoadedBlueprint = (*Provider)->LoadBlueprintProgram(BlueprintID);
	if (!LoadedBlueprint) return Fail(10, "load versioned cooked Blueprint");
	auto LoadedScene = (*Provider)->LoadScene(SceneID);
	if (!LoadedScene) return Fail(11, "load cooked scene");
	const auto Entity = (**LoadedScene).FindEntity(EntityID);
	if (!Entity) return Fail(12, "resolve cooked scene entity");
	const auto Component = Entity->GetBlueprint();
	if (!Component || !Component->has_value() || Component->value().BlueprintAsset != BlueprintID)
		return Fail(12, "resolve cooked scene Blueprint reference");
	const VerifiedBlueprintAsset& Blueprint = *LoadedBlueprint;
	const VerifiedBlueprintProgram* OnCreate = FindEventProgram(Blueprint, BlueprintEventKind::OnCreate);
	const VerifiedBlueprintProgram* OnTimer = FindEventProgram(Blueprint, BlueprintEventKind::OnTimer);
	const VerifiedBlueprintProgram* OnUpdate = FindEventProgram(Blueprint, BlueprintEventKind::OnUpdate);
	if (!OnCreate || !OnTimer || !OnUpdate) return Fail(13, "resolve cooked event bytecode");
	std::vector<float> ScalarState{ 0.0f, 0.0f };
	auto Scheduled = ExecuteBlueprintProgram(*OnCreate, ScalarState, BlueprintDefaultInstructionBudget);
	if (!Scheduled || Scheduled->TimerRequests.size() != 1 || Scheduled->TimerRequests.front().Name != "cooldown" ||
		Scheduled->TimerRequests.front().DelaySeconds != 0.05f)
		return Fail(14, "execute cooked timer scheduling bytecode");
	auto WrongTimer = ExecuteBlueprintProgram(*OnTimer, ScalarState, BlueprintDefaultInstructionBudget, "other");
	if (!WrongTimer || !WrongTimer->PropertyWrites.empty())
		return Fail(15, "ignore unrelated cooked timer callback");
	auto Fired = ExecuteBlueprintProgram(*OnTimer, ScalarState, BlueprintDefaultInstructionBudget, "cooldown");
	if (!Fired || Fired->PropertyWrites.size() != 1 || Fired->PropertyWrites.front().PropertyId != "health" ||
		Fired->PropertyWrites.front().Value != 7.0f)
		return Fail(16, "execute cooked timer callback bytecode");
	std::vector<float> EmptySpawnState{ 0.0f, 0.0f };
	BlueprintExecutionServices CookedQueryServices;
	CookedQueryServices.Raycast = [](const BlueprintRaycastQuery& Query) -> std::expected<bool, BlueprintProgramError>
	{
		return Query.OriginX == 0.0f && Query.OriginY == 0.0f && Query.OriginZ == 0.0f &&
			Query.DirectionX == 1.0f && Query.DirectionY == 0.0f && Query.DirectionZ == 0.0f &&
			Query.MaxDistance == 20.0f;
	};
	auto Spawned = ExecuteBlueprintProgram(*OnUpdate, EmptySpawnState, BlueprintDefaultInstructionBudget, {},
		CookedQueryServices);
	if (!Spawned || Spawned->EntitySpawnRequests.size() != 1 || Spawned->EntitySpawnRequests.front().X != 8.0f ||
		Spawned->EntitySpawnRequests.front().Y != 9.0f || Spawned->EntitySpawnRequests.front().Z != 10.0f ||
		Spawned->PropertyWrites.size() != 1 || Spawned->PropertyWrites.front().PropertyId != "raycast-hit" ||
		Spawned->PropertyWrites.front().Value != 1.0f)
		return Fail(17, "execute cooked Blueprint raycast and entity spawn bytecode");
	SceneRuntimeDesc RuntimeDescription;
	RuntimeDescription.Audio.OutputBackend = AudioOutputBackend::Null;
	PlayerSession Session(*Provider, RuntimeDescription);
	auto SessionStarted = Session.Start();
	if (!SessionStarted)
	{
		std::cerr << "Cooked PlayerSession start error: " << SessionStarted.error().Message << "\n";
		Session.Stop();
		return Fail(18, "start cooked-only PlayerSession");
	}
	if (Session.GetActiveSceneAsset() != SceneID)
	{
		std::cerr << "Cooked PlayerSession active scene expected=" << SceneID.ToString() << " actual=" <<
			(Session.GetActiveSceneAsset() ? Session.GetActiveSceneAsset()->ToString() : std::string("nil")) << " cooked=" <<
			Session.IsCookedSession() << " scene=" << (Session.GetActiveScene() != nullptr) << "\n";
		Session.Stop();
		return Fail(18, "verify cooked-only startup scene identity");
	}
	if (!Session.Advance(Timestep(1.0 / 60.0)) || Session.GetActiveSceneAsset() != SceneID)
		return Fail(19, "queue cooked Blueprint transition at frame boundary");
	const Scene* StartupScene = Session.GetActiveScene();
	const auto RuntimeEntity = StartupScene ? StartupScene->FindEntity(EntityID) : std::nullopt;
	const auto RuntimeBlueprint = RuntimeEntity ? RuntimeEntity->GetBlueprint() :
		std::expected<std::optional<BlueprintComponent>, SceneError>(std::optional<BlueprintComponent>{});
	if (!RuntimeBlueprint || !RuntimeBlueprint->has_value() ||
		RuntimeBlueprint->value().FindFloatProperty("raycast-hit") != 1.0f)
	{
		const auto PropertyValue = RuntimeBlueprint && RuntimeBlueprint->has_value()
			? RuntimeBlueprint->value().FindFloatProperty("raycast-hit") : std::nullopt;
		std::cerr << "Cooked Blueprint raycast result expected=1 actual=" <<
			(PropertyValue ? std::to_string(*PropertyValue) : std::string("missing")) <<
			" runtime_entity=" << RuntimeEntity.has_value() << " diagnostics=" << Session.GetBlueprintDiagnostics().size() << "\n";
		for (const BlueprintRuntimeDiagnostic& Diagnostic : Session.GetBlueprintDiagnostics())
			std::cerr << "Blueprint diagnostic entity=" << Diagnostic.Entity.ToString() << " code=" <<
				static_cast<uint32_t>(Diagnostic.Code) << " message=" << Diagnostic.Message << "\n";
		return Fail(19, "execute cooked Blueprint raycast in the Player runtime");
	}
	if (!Session.Advance(Timestep(1.0 / 60.0)) || Session.GetActiveSceneAsset() != DestinationSceneID)
		return Fail(20, "activate cooked Blueprint transition destination");
	const Scene* ActiveScene = Session.GetActiveScene();
	if (!ActiveScene || ActiveScene->GetEntities().size() != 1 ||
		ActiveScene->GetEntities().front().GetTag()->Name != "Cooked destination marker")
		return Fail(21, "verify cooked transition destination content");
	Session.Stop();
	return 0;
}
