#include "TestEngineSupport.h"

#include <unordered_map>

using namespace PulseForge;

namespace
{
	std::shared_ptr<const VerifiedBlueprintProgram> MakeBlueprintProgram(
		float Value,
		bool Initialize,
		TestRunner& Tests)
	{
		BlueprintProgramSource Source;
		Source.Constants = { Value };
		Source.ScalarPropertyIds = { "health" };
		if (Initialize)
		{
			Source.Instructions = {
				{ BlueprintOpcode::PushConstant, 0, 0, UUID(50, 1) },
				{ BlueprintOpcode::StoreScalar, 0, 0, UUID(50, 2) },
				{ BlueprintOpcode::LoadScalar, 0, 0, UUID(50, 3) },
				{ BlueprintOpcode::WriteScalarProperty, 0, 0, UUID(50, 4) },
				{ BlueprintOpcode::Return, 0, 0, UUID(50, 5) }
			};
		}
		else
		{
			Source.Instructions = {
				{ BlueprintOpcode::LoadScalar, 0, 0, UUID(50, 6) },
				{ BlueprintOpcode::PushConstant, 0, 0, UUID(50, 7) },
				{ BlueprintOpcode::Add, 0, 0, UUID(50, 8) },
				{ BlueprintOpcode::StoreScalar, 0, 0, UUID(50, 9) },
				{ BlueprintOpcode::LoadScalar, 0, 0, UUID(50, 10) },
				{ BlueprintOpcode::WriteScalarProperty, 0, 0, UUID(50, 11) },
				{ BlueprintOpcode::Return, 0, 0, UUID(50, 12) }
			};
		}
		auto Verified = VerifyBlueprintProgram(std::move(Source));
		PF_CHECK(Tests, Verified.has_value());
		if (!Verified) return {};
		return std::make_shared<VerifiedBlueprintProgram>(std::move(*Verified));
	}

	void TestSceneRuntimeBlueprintEvents(TestRunner& Tests)
	{
		std::error_code Error;
		const std::filesystem::path Temp = std::filesystem::temp_directory_path(Error);
		PF_CHECK(Tests, !Error);
		if (Error) return;
		const auto ProjectUUID = UUID::Generate();
		PF_CHECK(Tests, ProjectUUID.has_value());
		if (!ProjectUUID) return;
		const std::filesystem::path Root = Temp / ("PulseForgeBlueprintSceneRuntime-" + ProjectUUID->ToString());
		struct Cleanup final
		{
			std::filesystem::path Root;
			~Cleanup() { std::error_code Ignore; std::filesystem::remove_all(Root, Ignore); }
		} Cleanup{ Root };
		auto ProjectResult = Project::Create(Root / "BlueprintRuntime.pfproj", "Blueprint Runtime");
		PF_CHECK(Tests, ProjectResult.has_value());
		if (!ProjectResult) return;

		constexpr UUID ActorA{ 0x5000000000004000ull, 0x8000000000000001ull };
		constexpr UUID ActorB{ 0x5000000000004000ull, 0x8000000000000002ull };
		Scene TestScene;
		const auto First = TestScene.CreateEntityWithUUID(ActorA, "Blueprint actor A");
		const auto Second = TestScene.CreateEntityWithUUID(ActorB, "Blueprint actor B");
		PF_CHECK(Tests, First && Second);
		if (!First || !Second) return;

		const UUID ProgramAsset{ 0x5100000000004000ull, 0x8000000000000001ull };
		const auto OnCreate = MakeBlueprintProgram(5.0f, true, Tests);
		auto OnUpdate = MakeBlueprintProgram(1.0f, false, Tests);
		uint64_t UpdateRevision = 1;
		uint32_t OnCreateBindings = 0;
		SceneRuntimeServices Services;
		Services.ResolveBlueprintProgram = [&](const Entity& Target, BlueprintEventKind Event)
			-> std::optional<BlueprintProgramBinding>
		{
			if ((Target.GetUUID() != ActorA && Target.GetUUID() != ActorB) || Event == BlueprintEventKind::OnCreate)
			{
				if (Event == BlueprintEventKind::OnCreate && (Target.GetUUID() == ActorA || Target.GetUUID() == ActorB))
				{
					++OnCreateBindings;
					return BlueprintProgramBinding{ ProgramAsset, 1, OnCreate };
				}
				return std::nullopt;
			}
			if (Event == BlueprintEventKind::OnUpdate)
				return BlueprintProgramBinding{ ProgramAsset, UpdateRevision, OnUpdate };
			return std::nullopt;
		};
		Services.GetBlueprintProgramGeneration = [&] { return UpdateRevision; };
		std::unordered_map<UUID, float, UUIDHash> Values;
		bool RejectSecondActor = false;
		uint32_t RejectedActorWrites = 0;
		Services.ApplyBlueprintPropertyWrites = [&](const Entity& Target, std::span<const BlueprintScalarPropertyWrite> Writes)
		{
			if (RejectSecondActor && Target.GetUUID() == ActorB)
			{
				++RejectedActorWrites;
				return false;
			}
			if (!Target.IsValid() || Writes.empty()) return false;
			for (const BlueprintScalarPropertyWrite& Write : Writes)
				if (Write.PropertyId != "health" || !std::isfinite(Write.Value)) return false;
			for (const BlueprintScalarPropertyWrite& Write : Writes) Values[Target.GetUUID()] = Write.Value;
			return true;
		};

		SceneRuntimeDesc Description;
		Description.Audio.OutputBackend = AudioOutputBackend::Null;
		SceneRuntime Runtime(*ProjectResult, Description, Services);
		PF_CHECK(Tests, Runtime.Start(TestScene).has_value());
		PF_CHECK(Tests, Values[ActorA] == 5.0f && Values[ActorB] == 5.0f);
		PF_CHECK(Tests, OnCreateBindings == 2);
		PF_CHECK(Tests, Runtime.Advance(TestScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, Values[ActorA] == 6.0f && Values[ActorB] == 6.0f);
		PF_CHECK(Tests, Runtime.Advance(TestScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, Values[ActorA] == 7.0f && Values[ActorB] == 7.0f);

		OnUpdate = MakeBlueprintProgram(10.0f, false, Tests);
		++UpdateRevision;
		PF_CHECK(Tests, Runtime.Advance(TestScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, Values[ActorA] == 10.0f && Values[ActorB] == 10.0f);
		RejectSecondActor = true;
		PF_CHECK(Tests, Runtime.Advance(TestScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, Values[ActorA] == 20.0f && Values[ActorB] == 10.0f && RejectedActorWrites == 1);
		const auto Diagnostics = Runtime.GetBlueprintDiagnostics();
		PF_CHECK(Tests, Diagnostics.size() == 1 && Diagnostics[0].Entity == ActorB &&
			Diagnostics[0].Event == BlueprintEventKind::OnUpdate &&
			Diagnostics[0].Code == BlueprintProgramErrorCode::HostRejectedPropertyWrite &&
			Diagnostics[0].SourceNode == UUID(50, 11));
		PF_CHECK(Tests, Runtime.Advance(TestScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, Values[ActorA] == 30.0f && RejectedActorWrites == 1);

		PF_CHECK(Tests, TestScene.DestroyEntity(*Second).has_value());
		const auto Recreated = TestScene.CreateEntityWithUUID(ActorB, "Recreated Blueprint actor B");
		PF_CHECK(Tests, Recreated.has_value());
		RejectSecondActor = false;
		PF_CHECK(Tests, Runtime.Advance(TestScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, Values[ActorB] == 10.0f);
		PF_CHECK(Tests, OnCreateBindings == 3);
		Runtime.Stop();
	}
}

void RunBlueprintSceneRuntimeTests(TestRunner& Tests)
{
	TestSceneRuntimeBlueprintEvents(Tests);
}
