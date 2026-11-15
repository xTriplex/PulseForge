#include "Assets/CookedAssetCatalog.h"
#include "Runtime/CookedContentProvider.h"
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
	BlueprintProgramSource StartTimer;
	StartTimer.Version = BlueprintProgramFormatVersion;
	StartTimer.Constants = { 0.05f };
	StartTimer.ScalarPropertyIds = { "health" };
	StartTimer.TimerNames = { "cooldown" };
	StartTimer.Instructions = {
		{ BlueprintOpcode::RequestTimerSelf, 0, 0, UUID(71, 1) },
		{ BlueprintOpcode::Return, 0, 0, UUID(71, 2) }
	};
	BlueprintProgramSource TimerCallback;
	TimerCallback.Version = BlueprintProgramFormatVersion;
	TimerCallback.Constants = { 7.0f };
	TimerCallback.ScalarPropertyIds = { "health" };
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
	SpawnOnUpdate.Constants = { 8.0f, 9.0f, 10.0f };
	SpawnOnUpdate.Instructions = {
		{ BlueprintOpcode::PushConstant, 0, 0, UUID(74, 1) },
		{ BlueprintOpcode::PushConstant, 1, 0, UUID(74, 2) },
		{ BlueprintOpcode::PushConstant, 2, 0, UUID(74, 3) },
		{ BlueprintOpcode::RequestSpawnEntity, 0, 0, UUID(74, 4) },
		{ BlueprintOpcode::Return, 0, 0, UUID(74, 5) }
	};
	const std::array<BlueprintEventProgramSource, 3> BlueprintEventsWithSpawn{
		BlueprintEvents[0], BlueprintEvents[1],
		BlueprintEventProgramSource{ BlueprintEventKind::OnUpdate, SpawnOnUpdate }
	};
	auto BlueprintPayload = SerializeBlueprintAsset(BlueprintEventsWithSpawn);
	if (!BlueprintPayload) return Fail(3, "compile and serialize Blueprint event product");

	Scene AuthoredScene;
	const auto AuthoredEntity = AuthoredScene.CreateEntityWithUUID(EntityID, "Cooked Blueprint timer");
	if (!AuthoredEntity || !AuthoredEntity->SetBlueprint(BlueprintComponent{ BlueprintID, true, {} }))
		return Fail(4, "author scene Blueprint reference");
	const auto SceneText = SceneSerializer::Serialize(AuthoredScene);
	if (!SceneText) return Fail(5, "serialize fixture scene");
	const std::span<const std::byte> ScenePayload(reinterpret_cast<const std::byte*>(SceneText->data()), SceneText->size());

	CookedAssetEntry BlueprintEntry = MakeEntry(BlueprintID, CookedAssetProductKind::BlueprintProgram,
		BlueprintAssetFormatVersion, "b/blueprint.pfbp", *BlueprintPayload);
	CookedAssetEntry SceneEntry = MakeEntry(SceneID, CookedAssetProductKind::Scene, 1, "s/startup.pfscene", ScenePayload);
	if (!WritePayload(Root, BlueprintEntry, *BlueprintPayload) || !WritePayload(Root, SceneEntry, ScenePayload))
		return Fail(6, "write cooked payloads");
	const CookedAssetCompatibility Compatibility = GetCompatibility();
	const auto Manifest = CookedAssetCatalog::SerializeManifest(
		UUID(73, 1), SceneID, Compatibility, {}, { BlueprintEntry, SceneEntry });
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
	std::vector<float> ScalarState{ 0.0f };
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
	std::vector<float> EmptySpawnState;
	auto Spawned = ExecuteBlueprintProgram(*OnUpdate, EmptySpawnState, BlueprintDefaultInstructionBudget);
	if (!Spawned || Spawned->EntitySpawnRequests.size() != 1 || Spawned->EntitySpawnRequests.front().X != 8.0f ||
		Spawned->EntitySpawnRequests.front().Y != 9.0f || Spawned->EntitySpawnRequests.front().Z != 10.0f)
		return Fail(17, "execute cooked entity spawn bytecode");
	return 0;
}
