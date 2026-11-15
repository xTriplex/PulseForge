#include "TestEngineSupport.h"

#include "Assets/AssetOperations.h"
#include "Scene/Components/AudioListenerComponent.h"
#include "Scene/Components/AudioSourceComponent.h"
#include "Scene/Components/BoxColliderComponent.h"
#include "Scene/Components/RigidbodyComponent.h"
#include "Scene/Components/RuntimeUICanvasComponent.h"
#include "Scene/Components/ScriptComponent.h"
#include "UI/RuntimeUI.h"

#include <algorithm>
#include <array>
#include <unordered_map>

using namespace PulseForge;

namespace
{
	std::vector<std::byte> MakeBlueprintTestWave()
	{
		constexpr uint32_t SampleCount = 4410;
		constexpr uint32_t DataSize = SampleCount * 2;
		std::vector<std::byte> Data(44 + DataSize);
		const auto WriteFourCC = [&Data](size_t Offset, std::string_view Value)
		{
			for (size_t Index = 0; Index < Value.size(); ++Index)
				Data[Offset + Index] = static_cast<std::byte>(static_cast<unsigned char>(Value[Index]));
		};
		const auto Write16 = [&Data](size_t Offset, uint16_t Value)
		{
			Data[Offset] = static_cast<std::byte>(Value & 0xff);
			Data[Offset + 1] = static_cast<std::byte>((Value >> 8) & 0xff);
		};
		const auto Write32 = [&Data](size_t Offset, uint32_t Value)
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

		constexpr std::array<UUID, 6> Actors{
			UUID{ 0x5000000000004000ull, 0x8000000000000001ull }, UUID{ 0x5000000000004000ull, 0x8000000000000002ull },
			UUID{ 0x5000000000004000ull, 0x8000000000000003ull }, UUID{ 0x5000000000004000ull, 0x8000000000000004ull },
			UUID{ 0x5000000000004000ull, 0x8000000000000005ull }, UUID{ 0x5000000000004000ull, 0x8000000000000006ull }
		};
		const UUID ActorA = Actors[0];
		const UUID ActorB = Actors[1];
		const UUID ActorC = Actors[2];
		Scene TestScene;
		std::array<Entity, Actors.size()> ActorEntities;
		for (size_t Index = 0; Index < Actors.size(); ++Index)
		{
			auto Created = TestScene.CreateEntityWithUUID(Actors[Index], "Blueprint actor " + std::to_string(Index));
			PF_CHECK(Tests, Created.has_value());
			if (!Created) return;
			ActorEntities[Index] = *Created;
		}
		RuntimeUIDocument UIDocument;
		UIDocument.DesignSize = { 100.0f, 100.0f };
		RuntimeUIElement Button;
		Button.ID = UUID(0x5200000000004000ull, 0x8000000000000001ull);
		Button.Kind = RuntimeUIElementKind::Button;
		Button.OffsetMinimum = { 0.0f, 0.0f };
		Button.OffsetMaximum = { 100.0f, 100.0f };
		Button.Action = "Confirm";
		UIDocument.Elements.push_back(std::move(Button));
		const auto UISource = SerializeRuntimeUIDocument(UIDocument);
		PF_CHECK(Tests, UISource.has_value());
		if (!UISource) return;
		std::vector<std::byte> UIBytes(UISource->size());
		std::transform(UISource->begin(), UISource->end(), UIBytes.begin(),
			[](char Value) { return static_cast<std::byte>(static_cast<unsigned char>(Value)); });
		const auto UIAsset = AssetOperations::CreateAssetFromBytes(ProjectResult->GetAssetRegistry(),
			ProjectResult->GetRootPath(), UIBytes, "Assets/BlueprintActions.pfui");
		PF_CHECK(Tests, UIAsset.has_value());
		if (!UIAsset) return;
		constexpr std::string_view UIStopScript =
			"function OnUIAction(action)\n"
			"  local queued, message = scene.request_stop()\n"
			"  assert(queued, message)\n"
			"end\n";
		std::vector<std::byte> ScriptBytes(UIStopScript.size());
		std::transform(UIStopScript.begin(), UIStopScript.end(), ScriptBytes.begin(),
			[](char Value) { return static_cast<std::byte>(static_cast<unsigned char>(Value)); });
		const auto StopScriptAsset = AssetOperations::CreateAssetFromBytes(ProjectResult->GetAssetRegistry(),
			ProjectResult->GetRootPath(), ScriptBytes, "Assets/RequestStopOnUI.lua");
		PF_CHECK(Tests, StopScriptAsset.has_value());
		if (!StopScriptAsset) return;
		PF_CHECK(Tests, ActorEntities[0].SetRuntimeUICanvas(RuntimeUICanvasComponent{
			UIAsset->ID, true, RuntimeUIInputPolicy::Modal }).has_value());
		PF_CHECK(Tests, ActorEntities[0].SetScript(ScriptComponent{ StopScriptAsset->ID }).has_value());

		const UUID ProgramAsset{ 0x5100000000004000ull, 0x8000000000000001ull };
		const auto OnCreate = MakeBlueprintProgram(5.0f, true, Tests);
		const auto OnDestroy = MakeBlueprintProgram(99.0f, true, Tests);
		auto OnUpdate = MakeBlueprintProgram(1.0f, false, Tests);
		BlueprintProgramSource UIActionSource;
		UIActionSource.Constants = { 42.0f };
		UIActionSource.ScalarPropertyIds = { "health" };
		UIActionSource.ActionNames = { "Confirm" };
		UIActionSource.Instructions = {
			{ BlueprintOpcode::JumpIfAction, 0, 2, UUID(56, 1) },
			{ BlueprintOpcode::Return, 0, 0, UUID(56, 2) },
			{ BlueprintOpcode::PushConstant, 0, 0, UUID(56, 3) },
			{ BlueprintOpcode::WriteScalarProperty, 0, 0, UUID(56, 4) },
			{ BlueprintOpcode::Return, 0, 0, UUID(56, 5) }
		};
		const auto VerifiedUIAction = VerifyBlueprintProgram(std::move(UIActionSource));
		PF_CHECK(Tests, VerifiedUIAction.has_value());
		const auto UIAction = VerifiedUIAction
			? std::make_shared<const VerifiedBlueprintProgram>(std::move(*VerifiedUIAction))
			: std::shared_ptr<const VerifiedBlueprintProgram>{};
		auto MakeNamedAction = [&](float Value)
		{
			BlueprintProgramSource Source;
			Source.Constants = { Value };
			Source.ScalarPropertyIds = { "health" };
			Source.ActionNames = { "Confirm" };
			Source.Instructions = {
				{ BlueprintOpcode::JumpIfAction, 0, 2, UUID(57, 1) },
				{ BlueprintOpcode::Return, 0, 0, UUID(57, 2) },
				{ BlueprintOpcode::PushConstant, 0, 0, UUID(57, 3) },
				{ BlueprintOpcode::WriteScalarProperty, 0, 0, UUID(57, 4) },
				{ BlueprintOpcode::Return, 0, 0, UUID(57, 5) }
			};
			const auto Verified = VerifyBlueprintProgram(std::move(Source));
			PF_CHECK(Tests, Verified.has_value());
			return Verified ? std::make_shared<const VerifiedBlueprintProgram>(std::move(*Verified))
				: std::shared_ptr<const VerifiedBlueprintProgram>{};
		};
		const auto ActionPressed = MakeNamedAction(77.0f);
		const auto ActionReleased = MakeNamedAction(88.0f);
		const auto ActionCancelled = MakeNamedAction(99.0f);
		uint64_t UpdateRevision = 1;
		uint32_t OnCreateBindings = 0;
		SceneRuntimeServices Services;
		Services.QueueStop = [] { return true; };
		Services.ResolveBlueprintProgram = [&](const Entity& Target, BlueprintEventKind Event)
			-> std::optional<BlueprintProgramBinding>
		{
			const bool IsActor = std::find(Actors.begin(), Actors.end(), Target.GetUUID()) != Actors.end();
			if (!IsActor || Event == BlueprintEventKind::OnCreate || Event == BlueprintEventKind::OnDestroy)
			{
				if (Event == BlueprintEventKind::OnCreate && IsActor)
				{
					++OnCreateBindings;
					return BlueprintProgramBinding{ ProgramAsset, 1, OnCreate };
				}
				if (Event == BlueprintEventKind::OnDestroy && IsActor)
					return BlueprintProgramBinding{ ProgramAsset, 1, OnDestroy };
				return std::nullopt;
			}
			if (Event == BlueprintEventKind::OnUpdate && Target.GetUUID() != ActorC)
				return BlueprintProgramBinding{ ProgramAsset, UpdateRevision, OnUpdate };
			if (Event == BlueprintEventKind::OnUIAction && Target.GetUUID() == ActorA)
				return BlueprintProgramBinding{ ProgramAsset, 1, UIAction };
			if (Target.GetUUID() == ActorC && Event == BlueprintEventKind::OnActionPressed)
				return BlueprintProgramBinding{ ProgramAsset, 1, ActionPressed };
			if (Target.GetUUID() == ActorC && Event == BlueprintEventKind::OnActionReleased)
				return BlueprintProgramBinding{ ProgramAsset, 1, ActionReleased };
			if (Target.GetUUID() == ActorC && Event == BlueprintEventKind::OnActionCancelled)
				return BlueprintProgramBinding{ ProgramAsset, 1, ActionCancelled };
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

		GameplayInputService GameplayInput;
		GameplayActionMap ActionMap;
		GameplayActionDefinition ConfirmAction{ .Id = "Confirm", .Type = ActionValueType::Digital };
		ConfirmAction.Bindings.push_back(ActionBinding{ .Id = "Confirm.space", .Kind = ActionBindingKind::Button,
			.Primary = { InputDevice::Keyboard, static_cast<uint16_t>(InputKey::Space) } });
		ActionMap.Actions.push_back(std::move(ConfirmAction));
		PF_CHECK(Tests, GameplayInput.SetActionMap(std::move(ActionMap)).has_value());
		Services.GameplayInput = &GameplayInput;
		InputRoutingState Routing;
		Routing.FocusedWindow = 41;
		Routing.Contexts[0] = { .Context = InputContext::Gameplay, .Consumes = InputDeviceMask::All,
			.OwnerWindow = 41, .Active = true };
		Routing.ContextCount = 1;
		SceneRuntimeDesc Description;
		Description.Audio.OutputBackend = AudioOutputBackend::Null;
		SceneRuntime Runtime(*ProjectResult, Description, Services);
		PF_CHECK(Tests, Runtime.Start(TestScene).has_value());
		PF_CHECK(Tests, Values[ActorA] == 5.0f && Values[ActorB] == 5.0f);
		PF_CHECK(Tests, OnCreateBindings == Actors.size());
		PF_CHECK(Tests, Runtime.Advance(TestScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, Values[ActorA] == 6.0f && Values[ActorB] == 6.0f);
		PF_CHECK(Tests, Runtime.Advance(TestScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, Values[ActorA] == 7.0f && Values[ActorB] == 7.0f);
		PF_CHECK(Tests, Runtime.SetPaused(true).has_value());
		const RuntimeUIViewport Viewport{ { 100.0f, 100.0f }, 100, 100 };
		for (const RuntimeUIInputKind Kind : { RuntimeUIInputKind::PointerPressed, RuntimeUIInputKind::PointerReleased })
		{
			const RuntimeUIInputEvent InputEvent{ Kind, { 50.0f, 50.0f }, {} };
			PF_CHECK(Tests, Runtime.SubmitRuntimeUIFrame(Viewport, std::span(&InputEvent, 1)));
			PF_CHECK(Tests, Runtime.Advance(TestScene, Timestep(1.0 / 60.0)).has_value());
		}
		const auto UIStopRequest = Runtime.GetPendingBoundaryRequest();
		PF_CHECK(Tests, UIStopRequest && UIStopRequest->Kind == SceneRuntimeBoundaryRequestKind::Stop);
		PF_CHECK(Tests, Values[ActorA] == 7.0f);
		if (UIStopRequest)
			PF_CHECK(Tests, Runtime.ResolveBoundaryRequest(*UIStopRequest, SceneRuntimeBoundaryResolution::Cancelled));
		PF_CHECK(Tests, Runtime.SetPaused(false).has_value());

		OnUpdate = MakeBlueprintProgram(10.0f, false, Tests);
		++UpdateRevision;
		PF_CHECK(Tests, Runtime.Advance(TestScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, Values[ActorA] == 10.0f && Values[ActorB] == 10.0f);
		RejectSecondActor = true;
		const auto RejectedWriteFrame = Runtime.Advance(TestScene, Timestep(1.0 / 60.0));
		PF_CHECK(Tests, RejectedWriteFrame.has_value());
		PF_CHECK(Tests, RejectedWriteFrame && RejectedWriteFrame->BlueprintInstructionsExecuted == (Actors.size() - 1) * 7);
		PF_CHECK(Tests, Values[ActorA] == 20.0f && Values[ActorB] == 10.0f && RejectedActorWrites == 1);
		const auto Diagnostics = Runtime.GetBlueprintDiagnostics();
		PF_CHECK(Tests, Diagnostics.size() == 1 && Diagnostics[0].Entity == ActorB &&
			Diagnostics[0].Event == BlueprintEventKind::OnUpdate &&
			Diagnostics[0].Code == BlueprintProgramErrorCode::HostRejectedPropertyWrite &&
			Diagnostics[0].SourceNode == UUID(50, 11));
		PF_CHECK(Tests, Runtime.Advance(TestScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, Values[ActorA] == 30.0f && RejectedActorWrites == 1);

		PF_CHECK(Tests, TestScene.DestroyEntity(ActorEntities[1]).has_value());
		const auto Recreated = TestScene.CreateEntityWithUUID(ActorB, "Recreated Blueprint actor B");
		PF_CHECK(Tests, Recreated.has_value());
		RejectSecondActor = false;
		PF_CHECK(Tests, Runtime.Advance(TestScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, Values[ActorB] == 10.0f);
		PF_CHECK(Tests, OnCreateBindings == Actors.size() + 1);
		PF_CHECK(Tests, GameplayInput.QueueEvent({ .Kind = InputEventKind::Pressed,
			.Control = { InputDevice::Keyboard, static_cast<uint16_t>(InputKey::Space) }, .Window = 41 }));
		GameplayInput.PublishFrame(Routing);
		PF_CHECK(Tests, GameplayInput.FindAction("Confirm") && GameplayInput.FindAction("Confirm")->Pressed);
		PF_CHECK(Tests, Runtime.Advance(TestScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, Values[ActorC] == 77.0f);
		PF_CHECK(Tests, GameplayInput.QueueEvent({ .Kind = InputEventKind::Released,
			.Control = { InputDevice::Keyboard, static_cast<uint16_t>(InputKey::Space) }, .Window = 41 }));
		GameplayInput.PublishFrame(Routing);
		PF_CHECK(Tests, GameplayInput.FindAction("Confirm") && GameplayInput.FindAction("Confirm")->Released);
		PF_CHECK(Tests, Runtime.Advance(TestScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, Values[ActorC] == 88.0f);
		PF_CHECK(Tests, GameplayInput.QueueEvent({ .Kind = InputEventKind::Pressed,
			.Control = { InputDevice::Keyboard, static_cast<uint16_t>(InputKey::Space) }, .Window = 41 }));
		GameplayInput.PublishFrame(Routing);
		PF_CHECK(Tests, Runtime.Advance(TestScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, GameplayInput.QueueEvent({ .Kind = InputEventKind::FocusLost, .Window = 41 }));
		GameplayInput.PublishFrame(Routing);
		PF_CHECK(Tests, GameplayInput.FindAction("Confirm") && GameplayInput.FindAction("Confirm")->Canceled);
		PF_CHECK(Tests, Runtime.Advance(TestScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, Values[ActorC] == 99.0f);
		GameplayInput.PublishFrame(Routing);
		PF_CHECK(Tests, Runtime.Advance(TestScene, Timestep(1.0 / 60.0)).has_value());

		BlueprintProgramSource LoopSource;
		LoopSource.Constants = { 1.0f };
		LoopSource.Instructions = {
			{ BlueprintOpcode::PushConstant, 0, 0, UUID(55, 1) },
			{ BlueprintOpcode::JumpIfZero, 0, 3, UUID(55, 2) },
			{ BlueprintOpcode::Jump, 0, 0, UUID(55, 3) },
			{ BlueprintOpcode::Return, 0, 0, UUID(55, 4) }
		};
		const auto Loop = VerifyBlueprintProgram(std::move(LoopSource));
		PF_CHECK(Tests, Loop.has_value());
		if (Loop) OnUpdate = std::make_shared<VerifiedBlueprintProgram>(std::move(*Loop));
		++UpdateRevision;
		const auto BoundedFrame = Runtime.Advance(TestScene, Timestep(1.0 / 60.0));
		PF_CHECK(Tests, BoundedFrame && BoundedFrame->BlueprintInstructionsExecuted == 50'000);
		const auto BudgetDiagnostics = Runtime.GetBlueprintDiagnostics();
		PF_CHECK(Tests, BudgetDiagnostics.size() <= 256 && std::any_of(BudgetDiagnostics.begin(), BudgetDiagnostics.end(),
			[](const BlueprintRuntimeDiagnostic& Diagnostic)
			{
				return Diagnostic.Code == BlueprintProgramErrorCode::InstructionBudgetExceeded;
			}));
		Runtime.Stop();
		PF_CHECK(Tests, std::all_of(Actors.begin(), Actors.end(), [&](UUID Actor) { return Values[Actor] == 99.0f; }));
	}

	void TestSceneRuntimeBoundsGameplayActionFanout(TestRunner& Tests)
	{
		std::error_code Error;
		const auto ProjectId = UUID::Generate();
		PF_CHECK(Tests, ProjectId.has_value());
		if (!ProjectId) return;
		const std::filesystem::path Root = std::filesystem::temp_directory_path(Error) /
			("PulseForgeBlueprintActionBudget-" + ProjectId->ToString());
		struct Cleanup final
		{
			std::filesystem::path Root;
			~Cleanup() { std::error_code Ignore; std::filesystem::remove_all(Root, Ignore); }
		} Cleanup{ Root };
		auto ProjectResult = Project::Create(Root / "BlueprintActionBudget.pfproj", "Blueprint Action Budget");
		PF_CHECK(Tests, ProjectResult.has_value());
		if (!ProjectResult) return;

		Scene TestScene;
		for (uint64_t Index = 0; Index < 15; ++Index)
		{
			const UUID EntityId{ 0x5300000000004000ull, 0x8000000000000001ull + Index };
			PF_CHECK(Tests, TestScene.CreateEntityWithUUID(EntityId, "Action target").has_value());
		}
		constexpr UUID TriggerId{ 0x5300000000004000ull, 0x8000000000000100ull };
		constexpr UUID BodyId{ 0x5300000000004000ull, 0x8000000000000101ull };
		const auto Trigger = TestScene.CreateEntityWithUUID(TriggerId, "Action budget trigger");
		const auto Body = TestScene.CreateEntityWithUUID(BodyId, "Action budget body");
		PF_CHECK(Tests, Trigger.has_value() && Body.has_value());
		if (!Trigger || !Body) return;
		PF_CHECK(Tests, Trigger->SetRigidbody({ RigidbodyMotionType::Static }).has_value());
		BoxColliderComponent TriggerCollider;
		TriggerCollider.IsSensor = true;
		PF_CHECK(Tests, Trigger->SetCollider(TriggerCollider).has_value());
		PF_CHECK(Tests, Body->SetRigidbody({ RigidbodyMotionType::Dynamic, 1.0f, 0.5f, 0.0f, true }).has_value());
		PF_CHECK(Tests, Body->SetBoxCollider(BoxColliderComponent{}).has_value());
		BlueprintProgramSource Source;
		Source.Constants = { 1.0f };
		Source.ScalarPropertyIds = { "hit" };
		Source.ActionNames = { "Action0" };
		Source.Instructions = {
			{ BlueprintOpcode::JumpIfAction, 0, 2, UUID(58, 1) },
			{ BlueprintOpcode::Return, 0, 0, UUID(58, 2) },
			{ BlueprintOpcode::PushConstant, 0, 0, UUID(58, 3) },
			{ BlueprintOpcode::WriteScalarProperty, 0, 0, UUID(58, 4) },
			{ BlueprintOpcode::Return, 0, 0, UUID(58, 5) }
		};
		const auto Verified = VerifyBlueprintProgram(std::move(Source));
		PF_CHECK(Tests, Verified.has_value());
		if (!Verified) return;
		const auto Program = std::make_shared<const VerifiedBlueprintProgram>(std::move(*Verified));
		BlueprintProgramSource ContactSource;
		ContactSource.Constants = { 1.0f };
		ContactSource.ScalarPropertyIds = { "contact" };
		ContactSource.Instructions = {
			{ BlueprintOpcode::PushConstant, 0, 0, UUID(58, 10) },
			{ BlueprintOpcode::WriteScalarProperty, 0, 0, UUID(58, 11) },
			{ BlueprintOpcode::Return, 0, 0, UUID(58, 12) }
		};
		const auto VerifiedContact = VerifyBlueprintProgram(std::move(ContactSource));
		PF_CHECK(Tests, VerifiedContact.has_value());
		if (!VerifiedContact) return;
		const auto ContactProgram = std::make_shared<const VerifiedBlueprintProgram>(std::move(*VerifiedContact));
		constexpr AssetID ProgramAsset{ 0x5400000000004000ull, 0x8000000000000001ull };
		GameplayInputService GameplayInput;
		GameplayActionMap ActionMap;
		for (uint32_t Index = 0; Index < GameplayInputService::MaximumActions; ++Index)
		{
			GameplayActionDefinition Definition;
			Definition.Id = "Action" + std::to_string(Index);
			ActionBinding Binding;
			Binding.Id = Definition.Id + ".space";
			Binding.Primary = { InputDevice::Keyboard, static_cast<uint16_t>(InputKey::Space) };
			Definition.Bindings.push_back(std::move(Binding));
			ActionMap.Actions.push_back(std::move(Definition));
		}
		PF_CHECK(Tests, GameplayInput.SetActionMap(std::move(ActionMap)).has_value());
		SceneRuntimeServices Services;
		Services.GameplayInput = &GameplayInput;
		Services.ResolveBlueprintProgram = [&](const Entity& Target, BlueprintEventKind Event)
			-> std::optional<BlueprintProgramBinding>
		{
			if (Event == BlueprintEventKind::OnActionPressed)
				return BlueprintProgramBinding{ ProgramAsset, 1, Program };
			if ((Event == BlueprintEventKind::OnTriggerEnter) &&
				(Target.GetUUID() == TriggerId || Target.GetUUID() == BodyId))
				return BlueprintProgramBinding{ ProgramAsset, 2, ContactProgram };
			return std::nullopt;
		};
		uint32_t PropertyWrites = 0;
		uint32_t ContactWrites = 0;
		Services.ApplyBlueprintPropertyWrites = [&](const Entity& Target, std::span<const BlueprintScalarPropertyWrite> Writes)
		{
			if (!Target.IsValid() || Writes.size() != 1 || Writes[0].Value != 1.0f) return false;
			if (Writes[0].PropertyId == "hit") ++PropertyWrites;
			else if (Writes[0].PropertyId == "contact") ++ContactWrites;
			else return false;
			return true;
		};
		SceneRuntimeDesc Description;
		Description.Audio.OutputBackend = AudioOutputBackend::Null;
		SceneRuntime Runtime(*ProjectResult, Description, Services);
		PF_CHECK(Tests, Runtime.Start(TestScene).has_value());
		InputRoutingState Routing;
		Routing.FocusedWindow = 41;
		Routing.Contexts[0] = { .Context = InputContext::Gameplay, .Consumes = InputDeviceMask::All,
			.OwnerWindow = 41, .Active = true };
		Routing.ContextCount = 1;
		PF_CHECK(Tests, GameplayInput.QueueEvent({ .Kind = InputEventKind::Pressed,
			.Control = { InputDevice::Keyboard, static_cast<uint16_t>(InputKey::Space) }, .Window = 41 }));
		GameplayInput.PublishFrame(Routing);
		const auto Frame = Runtime.Advance(TestScene, Timestep(1.0 / 60.0));
		PF_CHECK(Tests, Frame && Frame->BlueprintInstructionsExecuted ==
			17u * 4u + (BlueprintMaximumEventDispatchesPerFrame - 17u) * 2u);
		PF_CHECK(Tests, PropertyWrites == 17);
		PF_CHECK(Tests, ContactWrites == 0);
		const auto Diagnostics = Runtime.GetBlueprintDiagnostics();
		PF_CHECK(Tests, std::any_of(Diagnostics.begin(), Diagnostics.end(), [](const BlueprintRuntimeDiagnostic& Diagnostic)
		{
			return Diagnostic.Event == BlueprintEventKind::OnActionPressed &&
				Diagnostic.Code == BlueprintProgramErrorCode::EventBudgetExceeded;
		}));
		GameplayInput.PublishFrame(Routing);
		PF_CHECK(Tests, Runtime.Advance(TestScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, ContactWrites == 2);
		Runtime.Stop();
	}

	void TestSceneRuntimeBlueprintContactEvents(TestRunner& Tests)
	{
		std::error_code Error;
		const auto ProjectId = UUID::Generate();
		PF_CHECK(Tests, ProjectId.has_value());
		if (!ProjectId) return;
		const std::filesystem::path Root = std::filesystem::temp_directory_path(Error) /
			("PulseForgeBlueprintContacts-" + ProjectId->ToString());
		struct Cleanup final
		{
			std::filesystem::path Root;
			~Cleanup() { std::error_code Ignore; std::filesystem::remove_all(Root, Ignore); }
		} Cleanup{ Root };
		auto ProjectResult = Project::Create(Root / "BlueprintContacts.pfproj", "Blueprint Contacts");
		PF_CHECK(Tests, ProjectResult.has_value());
		if (!ProjectResult) return;

		Scene TestScene;
		constexpr UUID TriggerId{ 0x5500000000004000ull, 0x8000000000000001ull };
		constexpr UUID BodyId{ 0x5500000000004000ull, 0x8000000000000002ull };
		const auto Trigger = TestScene.CreateEntityWithUUID(TriggerId, "Blueprint trigger");
		const auto Body = TestScene.CreateEntityWithUUID(BodyId, "Blueprint body");
		PF_CHECK(Tests, Trigger.has_value() && Body.has_value());
		if (!Trigger || !Body) return;
		PF_CHECK(Tests, Trigger->SetRigidbody({ RigidbodyMotionType::Static }).has_value());
		BoxColliderComponent TriggerCollider;
		TriggerCollider.IsSensor = true;
		PF_CHECK(Tests, Trigger->SetCollider(TriggerCollider).has_value());
		PF_CHECK(Tests, Body->SetRigidbody({ RigidbodyMotionType::Dynamic, 1.0f, 0.5f, 0.0f, true }).has_value());
		PF_CHECK(Tests, Body->SetBoxCollider(BoxColliderComponent{}).has_value());

		auto MakePhaseProgram = [&](float Phase)
		{
			BlueprintProgramSource Source;
			Source.Constants = { Phase };
			Source.ScalarPropertyIds = { "phase" };
			Source.Instructions = {
				{ BlueprintOpcode::PushConstant, 0, 0, UUID(59, 1) },
				{ BlueprintOpcode::WriteScalarProperty, 0, 0, UUID(59, 2) },
				{ BlueprintOpcode::Return, 0, 0, UUID(59, 3) }
			};
			const auto Verified = VerifyBlueprintProgram(std::move(Source));
			PF_CHECK(Tests, Verified.has_value());
			return Verified ? std::make_shared<const VerifiedBlueprintProgram>(std::move(*Verified))
				: std::shared_ptr<const VerifiedBlueprintProgram>{};
		};
		const auto EnterProgram = MakePhaseProgram(1.0f);
		const auto StayProgram = MakePhaseProgram(2.0f);
		const auto ExitProgram = MakePhaseProgram(3.0f);
		constexpr AssetID ProgramAsset{ 0x5600000000004000ull, 0x8000000000000001ull };
		SceneRuntimeServices Services;
		Services.ResolveBlueprintProgram = [&](const Entity& Target, BlueprintEventKind Event)
			-> std::optional<BlueprintProgramBinding>
		{
			if ((Target.GetUUID() != TriggerId && Target.GetUUID() != BodyId) ||
				(Event != BlueprintEventKind::OnTriggerEnter && Event != BlueprintEventKind::OnTriggerStay &&
					Event != BlueprintEventKind::OnTriggerExit)) return std::nullopt;
			const auto Program = Event == BlueprintEventKind::OnTriggerEnter ? EnterProgram
				: Event == BlueprintEventKind::OnTriggerStay ? StayProgram : ExitProgram;
			return BlueprintProgramBinding{ ProgramAsset, 1, Program };
		};
		std::unordered_map<UUID, float, UUIDHash> Phases;
		Services.ApplyBlueprintPropertyWrites = [&](const Entity& Target, std::span<const BlueprintScalarPropertyWrite> Writes)
		{
			if (Writes.size() != 1 || Writes[0].PropertyId != "phase" || !std::isfinite(Writes[0].Value)) return false;
			Phases[Target.GetUUID()] = Writes[0].Value;
			return true;
		};
		SceneRuntimeDesc Description;
		Description.Audio.OutputBackend = AudioOutputBackend::Null;
		SceneRuntime Runtime(*ProjectResult, Description, Services);
		PF_CHECK(Tests, Runtime.Start(TestScene).has_value());
		PF_CHECK(Tests, Runtime.Advance(TestScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, Phases[TriggerId] == 1.0f && Phases[BodyId] == 1.0f);
		PF_CHECK(Tests, Runtime.Advance(TestScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, Phases[TriggerId] == 2.0f && Phases[BodyId] == 2.0f);
		TransformComponent Away;
		Away.Translation.x = 5.0f;
		PF_CHECK(Tests, Body->SetTransform(Away).has_value());
		PF_CHECK(Tests, Runtime.Advance(TestScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, Phases[TriggerId] == 3.0f && Phases[BodyId] == 3.0f);
		Runtime.Stop();

		TransformComponent OverlapAgain;
		PF_CHECK(Tests, Body->SetTransform(OverlapAgain).has_value());
		BlueprintProgramSource StopSource;
		StopSource.Instructions = {
			{ BlueprintOpcode::RequestStop, 0, 0, UUID(59, 10) },
			{ BlueprintOpcode::Return, 0, 0, UUID(59, 11) }
		};
		const auto VerifiedStop = VerifyBlueprintProgram(std::move(StopSource));
		PF_CHECK(Tests, VerifiedStop.has_value());
		if (!VerifiedStop) return;
		const auto StopProgram = std::make_shared<const VerifiedBlueprintProgram>(std::move(*VerifiedStop));
		uint32_t StopBindings = 0;
		uint32_t StopQueueCalls = 0;
		SceneRuntimeServices StopServices;
		StopServices.ResolveBlueprintProgram = [&](const Entity&, BlueprintEventKind Event)
			-> std::optional<BlueprintProgramBinding>
		{
			if (Event != BlueprintEventKind::OnTriggerEnter) return std::nullopt;
			++StopBindings;
			return BlueprintProgramBinding{ ProgramAsset, 2, StopProgram };
		};
		StopServices.QueueStop = [&]
		{
			++StopQueueCalls;
			return true;
		};
		SceneRuntime StopRuntime(*ProjectResult, Description, StopServices);
		PF_CHECK(Tests, StopRuntime.Start(TestScene).has_value());
		const auto StopFrame = StopRuntime.Advance(TestScene, Timestep(1.0 / 60.0));
		PF_CHECK(Tests, StopFrame && StopFrame->BlueprintInstructionsExecuted == 2 &&
			StopBindings == 1 && StopQueueCalls == 1);
		const auto StopRequest = StopRuntime.GetPendingBoundaryRequest();
		PF_CHECK(Tests, StopRequest && StopRequest->Kind == SceneRuntimeBoundaryRequestKind::Stop);
		StopRuntime.Stop();

		Scene StaleContactScene;
		const auto StaleTrigger = StaleContactScene.CreateEntityWithUUID(TriggerId, "Stale Blueprint trigger");
		const auto StaleBody = StaleContactScene.CreateEntityWithUUID(BodyId, "Stale Blueprint body");
		PF_CHECK(Tests, StaleTrigger && StaleBody);
		if (!StaleTrigger || !StaleBody) return;
		PF_CHECK(Tests, StaleTrigger->SetRigidbody({ RigidbodyMotionType::Static }).has_value());
		PF_CHECK(Tests, StaleTrigger->SetCollider(TriggerCollider).has_value());
		PF_CHECK(Tests, StaleBody->SetRigidbody({ RigidbodyMotionType::Dynamic, 1.0f, 0.5f, 0.0f, true }).has_value());
		PF_CHECK(Tests, StaleBody->SetBoxCollider(BoxColliderComponent{}).has_value());
		std::vector<Entity> DispatchBudgetEntities;
		DispatchBudgetEntities.reserve(BlueprintMaximumEventDispatchesPerFrame - 2);
		for (uint64_t Index = 0; Index < BlueprintMaximumEventDispatchesPerFrame - 2; ++Index)
		{
			const UUID Identifier{ 0x5d00000000004000ull, 0x8000000000000001ull + Index };
			const auto Created = StaleContactScene.CreateEntityWithUUID(Identifier, "Contact dispatch budget filler");
			PF_CHECK(Tests, Created.has_value());
			if (!Created) return;
			DispatchBudgetEntities.push_back(*Created);
		}
		uint32_t StaleContactUpdateResolutions = 0;
		SceneRuntimeServices StaleContactServices;
		StaleContactServices.ResolveBlueprintProgram = [&](const Entity& Target, BlueprintEventKind Event)
			-> std::optional<BlueprintProgramBinding>
		{
			if (Event == BlueprintEventKind::OnUpdate) ++StaleContactUpdateResolutions;
			if ((Target.GetUUID() != TriggerId && Target.GetUUID() != BodyId) ||
				(Event != BlueprintEventKind::OnTriggerEnter && Event != BlueprintEventKind::OnTriggerStay &&
					Event != BlueprintEventKind::OnTriggerExit)) return std::nullopt;
			const auto Program = Event == BlueprintEventKind::OnTriggerEnter ? EnterProgram
				: Event == BlueprintEventKind::OnTriggerStay ? StayProgram : ExitProgram;
			return BlueprintProgramBinding{ ProgramAsset, 1, Program };
		};
		StaleContactServices.ApplyBlueprintPropertyWrites = Services.ApplyBlueprintPropertyWrites;
		SceneRuntime StaleContactRuntime(*ProjectResult, Description, StaleContactServices);
		PF_CHECK(Tests, StaleContactRuntime.Start(StaleContactScene).has_value());
		StaleContactUpdateResolutions = 0;
		Phases.clear();
		const auto BackloggedContactFrame = StaleContactRuntime.Advance(StaleContactScene, Timestep(1.0 / 60.0));
		PF_CHECK(Tests, BackloggedContactFrame && BackloggedContactFrame->PhysicsSteps > 0 &&
			BackloggedContactFrame->BlueprintInstructionsExecuted == 0 &&
			StaleContactUpdateResolutions == BlueprintMaximumEventDispatchesPerFrame && Phases.empty());
		PF_CHECK(Tests, StaleContactScene.DestroyEntity(*StaleTrigger).has_value());
		const auto ReplacementTrigger = StaleContactScene.CreateEntityWithUUID(TriggerId, "Replacement trigger without physics");
		PF_CHECK(Tests, ReplacementTrigger && *ReplacementTrigger != *StaleTrigger);
		for (const Entity& Filler : DispatchBudgetEntities)
			PF_CHECK(Tests, StaleContactScene.DestroyEntity(Filler).has_value());
		StaleContactUpdateResolutions = 0;
		const auto StaleContactDrainFrame = StaleContactRuntime.Advance(StaleContactScene, Timestep(1.0 / 60.0));
		PF_CHECK(Tests, StaleContactDrainFrame.has_value() && ReplacementTrigger->IsValid() && StaleBody->IsValid());
		PF_CHECK(Tests, StaleContactUpdateResolutions < BlueprintMaximumEventDispatchesPerFrame && Phases.empty());
		StaleContactRuntime.Stop();
	}

	void TestSceneRuntimeBlueprintDeferredStop(TestRunner& Tests)
	{
		std::error_code Error;
		const auto ProjectId = UUID::Generate();
		PF_CHECK(Tests, ProjectId.has_value());
		if (!ProjectId) return;
		const std::filesystem::path Root = std::filesystem::temp_directory_path(Error) /
			("PulseForgeBlueprintDeferredStop-" + ProjectId->ToString());
		struct Cleanup final
		{
			std::filesystem::path Root;
			~Cleanup() { std::error_code Ignore; std::filesystem::remove_all(Root, Ignore); }
		} Cleanup{ Root };
		auto ProjectResult = Project::Create(Root / "BlueprintDeferredStop.pfproj", "Blueprint Deferred Stop");
		PF_CHECK(Tests, ProjectResult.has_value());
		if (!ProjectResult) return;

		Scene TestScene;
		constexpr UUID StopperId{ 0x5700000000004000ull, 0x8000000000000001ull };
		constexpr UUID LaterId{ 0x5700000000004000ull, 0x8000000000000002ull };
		PF_CHECK(Tests, TestScene.CreateEntityWithUUID(StopperId, "Stop request source").has_value());
		PF_CHECK(Tests, TestScene.CreateEntityWithUUID(LaterId, "Later entity").has_value());
		BlueprintProgramSource Source;
		Source.Instructions = {
			{ BlueprintOpcode::RequestStop, 0, 0, UUID(60, 1) },
			{ BlueprintOpcode::RequestPause, 1, 0, UUID(60, 2) },
			{ BlueprintOpcode::Return, 0, 0, UUID(60, 3) }
		};
		const auto Verified = VerifyBlueprintProgram(std::move(Source));
		PF_CHECK(Tests, Verified.has_value());
		if (!Verified) return;
		const auto Program = std::make_shared<const VerifiedBlueprintProgram>(std::move(*Verified));
		constexpr AssetID ProgramAsset{ 0x5800000000004000ull, 0x8000000000000001ull };
		uint32_t UpdateResolutionAttempts = 0;
		uint32_t QueueAttempts = 0;
		bool AcceptStop = true;
		SceneRuntimeServices Services;
		Services.ResolveBlueprintProgram = [&](const Entity& Target, BlueprintEventKind Event)
			-> std::optional<BlueprintProgramBinding>
		{
			if (Event == BlueprintEventKind::OnUpdate) ++UpdateResolutionAttempts;
			if (Event != BlueprintEventKind::OnUpdate || Target.GetUUID() != StopperId) return std::nullopt;
			return BlueprintProgramBinding{ ProgramAsset, 1, Program };
		};
		Services.QueueStop = [&]
		{
			++QueueAttempts;
			return AcceptStop;
		};
		SceneRuntimeDesc Description;
		Description.Audio.OutputBackend = AudioOutputBackend::Null;
		SceneRuntime Runtime(*ProjectResult, Description, Services);
		PF_CHECK(Tests, Runtime.Start(TestScene).has_value());
		const auto Frame = Runtime.Advance(TestScene, Timestep(1.0 / 60.0));
		PF_CHECK(Tests, Frame && Frame->BlueprintInstructionsExecuted == 3 && UpdateResolutionAttempts == 1 &&
			QueueAttempts == 1 && !Runtime.IsPaused());
		const auto Pending = Runtime.GetPendingBoundaryRequest();
		PF_CHECK(Tests, Pending && Pending->Kind == SceneRuntimeBoundaryRequestKind::Stop && !Pending->Scene);
		PF_CHECK(Tests, Runtime.ResolveBoundaryRequest(*Pending, SceneRuntimeBoundaryResolution::Cancelled));
		PF_CHECK(Tests, !Runtime.GetPendingBoundaryRequest().has_value());
		AcceptStop = false;
		PF_CHECK(Tests, Runtime.Advance(TestScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, QueueAttempts == 2 && UpdateResolutionAttempts == 2 &&
			!Runtime.GetPendingBoundaryRequest().has_value());
		const auto Diagnostics = Runtime.GetBlueprintDiagnostics();
		PF_CHECK(Tests, Diagnostics.size() == 1 && Diagnostics[0].Entity == StopperId &&
			Diagnostics[0].Code == BlueprintProgramErrorCode::HostRejectedDeferredCall &&
			Diagnostics[0].SourceNode == UUID(60, 1));
		Runtime.Stop();

		Scene PauseScene;
		constexpr UUID PauseTarget{ 0x5900000000004000ull, 0x8000000000000001ull };
		PF_CHECK(Tests, PauseScene.CreateEntityWithUUID(PauseTarget, "Pause request source").has_value());
		BlueprintProgramSource PauseSource;
		PauseSource.Instructions = {
			{ BlueprintOpcode::RequestPause, 1, 0, UUID(61, 1) },
			{ BlueprintOpcode::Return, 0, 0, UUID(61, 2) }
		};
		const auto VerifiedPause = VerifyBlueprintProgram(std::move(PauseSource));
		PF_CHECK(Tests, VerifiedPause.has_value());
		if (!VerifiedPause) return;
		const auto PauseProgram = std::make_shared<const VerifiedBlueprintProgram>(std::move(*VerifiedPause));
		SceneRuntimeServices PauseServices;
		PauseServices.ResolveBlueprintProgram = [&](const Entity& Target, BlueprintEventKind Event)
			-> std::optional<BlueprintProgramBinding>
		{
			if (Target.GetUUID() != PauseTarget || Event != BlueprintEventKind::OnUpdate) return std::nullopt;
			return BlueprintProgramBinding{ ProgramAsset, 3, PauseProgram };
		};
		SceneRuntime PauseRuntime(*ProjectResult, Description, PauseServices);
		PF_CHECK(Tests, PauseRuntime.Start(PauseScene).has_value());
		const auto PauseFrame = PauseRuntime.Advance(PauseScene, Timestep(1.0 / 60.0));
		PF_CHECK(Tests, PauseFrame && PauseFrame->BlueprintInstructionsExecuted == 2 && PauseRuntime.IsPaused());
		const auto PausedFrame = PauseRuntime.Advance(PauseScene, Timestep(1.0 / 60.0));
		PF_CHECK(Tests, PausedFrame && PausedFrame->BlueprintInstructionsExecuted == 0 && PauseRuntime.IsPaused());
		PauseRuntime.Stop();

		Scene TransitionSceneState;
		constexpr UUID TransitionTarget{ 0x5a00000000004000ull, 0x8000000000000001ull };
		PF_CHECK(Tests, TransitionSceneState.CreateEntityWithUUID(TransitionTarget, "Transition request source").has_value());
		constexpr AssetID TransitionAsset{ 0x5b00000000004000ull, 0x8000000000000001ull };
		BlueprintProgramSource TransitionSource;
		TransitionSource.SceneTransitionAssets = { TransitionAsset };
		TransitionSource.Instructions = {
			{ BlueprintOpcode::RequestSceneTransition, 0, 0, UUID(62, 1) },
			{ BlueprintOpcode::Return, 0, 0, UUID(62, 2) }
		};
		const auto VerifiedTransition = VerifyBlueprintProgram(std::move(TransitionSource));
		PF_CHECK(Tests, VerifiedTransition.has_value());
		if (!VerifiedTransition) return;
		const auto TransitionProgram = std::make_shared<const VerifiedBlueprintProgram>(std::move(*VerifiedTransition));
		uint32_t TransitionRequests = 0;
		SceneRuntimeServices TransitionServices;
		TransitionServices.ResolveBlueprintProgram = [&](const Entity& Target, BlueprintEventKind Event)
			-> std::optional<BlueprintProgramBinding>
		{
			if (Target.GetUUID() != TransitionTarget || Event != BlueprintEventKind::OnUpdate) return std::nullopt;
			return BlueprintProgramBinding{ ProgramAsset, 4, TransitionProgram };
		};
		TransitionServices.QueueSceneTransition = [&](const AssetID& Requested)
		{
			++TransitionRequests;
			return Requested == TransitionAsset;
		};
		SceneRuntime TransitionRuntime(*ProjectResult, Description, TransitionServices);
		PF_CHECK(Tests, TransitionRuntime.Start(TransitionSceneState).has_value());
		const auto TransitionFrame = TransitionRuntime.Advance(TransitionSceneState, Timestep(1.0 / 60.0));
		PF_CHECK(Tests, TransitionFrame && TransitionFrame->BlueprintInstructionsExecuted == 2 && TransitionRequests == 1);
		const auto TransitionRequest = TransitionRuntime.GetPendingBoundaryRequest();
		PF_CHECK(Tests, TransitionRequest && TransitionRequest->Kind == SceneRuntimeBoundaryRequestKind::SceneTransition &&
			TransitionRequest->Scene == TransitionAsset);
		if (TransitionRequest)
			PF_CHECK(Tests, TransitionRuntime.ResolveBoundaryRequest(
				*TransitionRequest, SceneRuntimeBoundaryResolution::Rejected));
		TransitionRuntime.Stop();

		Scene DestroyScene;
		constexpr UUID DestroyTarget{ 0x5c00000000004000ull, 0x8000000000000001ull };
		const auto DestroyEntity = DestroyScene.CreateEntityWithUUID(DestroyTarget, "Deferred destruction target");
		PF_CHECK(Tests, DestroyEntity.has_value());
		BlueprintProgramSource DestroySource;
		DestroySource.Instructions = {
			{ BlueprintOpcode::RequestDestroySelf, 0, 0, UUID(63, 1) },
			{ BlueprintOpcode::Return, 0, 0, UUID(63, 2) }
		};
		const auto VerifiedDestroy = VerifyBlueprintProgram(std::move(DestroySource));
		PF_CHECK(Tests, VerifiedDestroy.has_value());
		if (!DestroyEntity || !VerifiedDestroy) return;
		const auto DestroyProgram = std::make_shared<const VerifiedBlueprintProgram>(std::move(*VerifiedDestroy));
		BlueprintProgramSource DestroyCallbackSource;
		DestroyCallbackSource.Constants = { 7.0f };
		DestroyCallbackSource.ScalarPropertyIds = { "destroyed" };
		DestroyCallbackSource.Instructions = {
			{ BlueprintOpcode::PushConstant, 0, 0, UUID(64, 1) },
			{ BlueprintOpcode::WriteScalarProperty, 0, 0, UUID(64, 2) },
			{ BlueprintOpcode::Return, 0, 0, UUID(64, 3) }
		};
		const auto VerifiedDestroyCallback = VerifyBlueprintProgram(std::move(DestroyCallbackSource));
		PF_CHECK(Tests, VerifiedDestroyCallback.has_value());
		if (!VerifiedDestroyCallback) return;
		const auto DestroyCallbackProgram = std::make_shared<const VerifiedBlueprintProgram>(std::move(*VerifiedDestroyCallback));
		uint32_t DestroyCallbacks = 0;
		SceneRuntimeServices DestroyServices;
		DestroyServices.ResolveBlueprintProgram = [&](const Entity& Target, BlueprintEventKind Event)
			-> std::optional<BlueprintProgramBinding>
		{
			if (Target.GetUUID() != DestroyTarget) return std::nullopt;
			if (Event == BlueprintEventKind::OnUpdate) return BlueprintProgramBinding{ ProgramAsset, 5, DestroyProgram };
			if (Event == BlueprintEventKind::OnDestroy) return BlueprintProgramBinding{ ProgramAsset, 6, DestroyCallbackProgram };
			return std::nullopt;
		};
		DestroyServices.ApplyBlueprintPropertyWrites = [&](const Entity& Target,
			std::span<const BlueprintScalarPropertyWrite> Writes)
		{
			if (Target != *DestroyEntity || Writes.size() != 1 || Writes.front().PropertyId != "destroyed" ||
				Writes.front().Value != 7.0f) return false;
			++DestroyCallbacks;
			return true;
		};
		SceneRuntime DestroyRuntime(*ProjectResult, Description, DestroyServices);
		PF_CHECK(Tests, DestroyRuntime.Start(DestroyScene).has_value());
		const auto DestroyFrame = DestroyRuntime.Advance(DestroyScene, Timestep(1.0 / 60.0));
		PF_CHECK(Tests, DestroyFrame && DestroyFrame->BlueprintInstructionsExecuted == 5 && DestroyCallbacks == 1 &&
			!DestroyEntity->IsValid());
		DestroyRuntime.Stop();

		BlueprintProgramSource ForceSource;
		ForceSource.Constants = { 0.0f, 120.0f, 0.0f };
		ForceSource.Instructions = {
			{ BlueprintOpcode::PushConstant, 0, 0, UUID(65, 1) },
			{ BlueprintOpcode::PushConstant, 1, 0, UUID(65, 2) },
			{ BlueprintOpcode::PushConstant, 2, 0, UUID(65, 3) },
			{ BlueprintOpcode::RequestApplyForceSelf, 0, 0, UUID(65, 4) },
			{ BlueprintOpcode::Return, 0, 0, UUID(65, 5) }
		};
		const auto VerifiedForce = VerifyBlueprintProgram(std::move(ForceSource));
		PF_CHECK(Tests, VerifiedForce.has_value());
		if (!VerifiedForce) return;
		const auto ForceProgram = std::make_shared<const VerifiedBlueprintProgram>(std::move(*VerifiedForce));
		SceneRuntimeDesc ForceDescription = Description;
		ForceDescription.Physics.Gravity = glm::vec3(0.0f);
		constexpr UUID ForceTarget{ 0x5d00000000004000ull, 0x8000000000000001ull };
		const auto AddPhysicsBody = [&](Scene& TargetScene, UUID Identifier, float X = 0.0f)
		{
			const auto Created = TargetScene.CreateEntityWithUUID(Identifier, "Blueprint force target");
			PF_CHECK(Tests, Created.has_value());
			if (!Created) return Entity{};
			PF_CHECK(Tests, Created->SetRigidbody({ RigidbodyMotionType::Dynamic, 1.0f }).has_value());
			PF_CHECK(Tests, Created->SetBoxCollider(BoxColliderComponent{}).has_value());
			if (X != 0.0f)
			{
				TransformComponent Transform;
				Transform.Translation.x = X;
				PF_CHECK(Tests, Created->SetTransform(Transform).has_value());
			}
			return *Created;
		};
		Scene ForceScene;
		const Entity ForceEntity = AddPhysicsBody(ForceScene, ForceTarget);
		if (!ForceEntity.IsValid()) return;
		SceneRuntimeServices ForceServices;
		ForceServices.ResolveBlueprintProgram = [&](const Entity& Target, BlueprintEventKind Event)
			-> std::optional<BlueprintProgramBinding>
		{
			if (Target != ForceEntity || Event != BlueprintEventKind::OnCreate) return std::nullopt;
			return BlueprintProgramBinding{ ProgramAsset, 7, ForceProgram };
		};
		SceneRuntime ForceRuntime(*ProjectResult, ForceDescription, ForceServices);
		PF_CHECK(Tests, ForceRuntime.Start(ForceScene).has_value());
		const auto ForceFrame = ForceRuntime.Advance(ForceScene, Timestep(1.0 / 60.0));
		const auto ForcePosition = ForceEntity.GetTransform();
		PF_CHECK(Tests, ForceFrame.has_value());
		if (ForceFrame) PF_CHECK(Tests, ForceFrame->BlueprintInstructionsExecuted == 0);
		PF_CHECK(Tests, ForcePosition.has_value());
		if (ForcePosition) PF_CHECK(Tests, ForcePosition->Translation.y > 0.0f);
		PF_CHECK(Tests, ForceRuntime.GetBlueprintDiagnostics().empty());
		ForceRuntime.Stop();

		Scene StaleForceScene;
		const Entity StaleForceEntity = AddPhysicsBody(StaleForceScene, ForceTarget);
		if (!StaleForceEntity.IsValid()) return;
		SceneRuntimeServices StaleForceServices;
		StaleForceServices.ResolveBlueprintProgram = [&](const Entity& Target, BlueprintEventKind Event)
			-> std::optional<BlueprintProgramBinding>
		{
			if (Target != StaleForceEntity || Event != BlueprintEventKind::OnCreate) return std::nullopt;
			return BlueprintProgramBinding{ ProgramAsset, 8, ForceProgram };
		};
		SceneRuntime StaleForceRuntime(*ProjectResult, ForceDescription, StaleForceServices);
		PF_CHECK(Tests, StaleForceRuntime.Start(StaleForceScene).has_value());
		PF_CHECK(Tests, StaleForceScene.DestroyEntity(StaleForceEntity).has_value());
		const Entity ReplacementForceEntity = AddPhysicsBody(StaleForceScene, ForceTarget);
		PF_CHECK(Tests, ReplacementForceEntity.IsValid() && ReplacementForceEntity != StaleForceEntity);
		const auto StaleForceFrame = StaleForceRuntime.Advance(StaleForceScene, Timestep(1.0 / 60.0));
		const auto ReplacementPosition = ReplacementForceEntity.GetTransform();
		PF_CHECK(Tests, StaleForceFrame.has_value());
		PF_CHECK(Tests, ReplacementPosition.has_value());
		if (ReplacementPosition) PF_CHECK(Tests, ReplacementPosition->Translation.y == 0.0f);
		PF_CHECK(Tests, StaleForceRuntime.GetBlueprintDiagnostics().empty());
		StaleForceRuntime.Stop();

		BlueprintProgramSource BulkForceSource;
		BulkForceSource.Constants = { 0.0f, 1.0f, 0.0f };
		for (size_t Index = 0; Index < BlueprintMaximumDeferredPhysicsCallsPerEvent - 1; ++Index)
		{
			BulkForceSource.Instructions.push_back({ BlueprintOpcode::PushConstant, 0, 0, UUID(66, Index * 4 + 1) });
			BulkForceSource.Instructions.push_back({ BlueprintOpcode::PushConstant, 1, 0, UUID(66, Index * 4 + 2) });
			BulkForceSource.Instructions.push_back({ BlueprintOpcode::PushConstant, 2, 0, UUID(66, Index * 4 + 3) });
			BulkForceSource.Instructions.push_back({ BlueprintOpcode::RequestApplyForceSelf, 0, 0, UUID(66, Index * 4 + 4) });
		}
		BulkForceSource.Instructions.push_back({ BlueprintOpcode::Return, 0, 0, UUID(66, 2000) });
		const auto VerifiedBulkForce = VerifyBlueprintProgram(std::move(BulkForceSource));
		PF_CHECK(Tests, VerifiedBulkForce.has_value());
		if (!VerifiedBulkForce) return;
		const auto BulkForceProgram = std::make_shared<const VerifiedBlueprintProgram>(std::move(*VerifiedBulkForce));
		BlueprintProgramSource OverflowForceSource;
		OverflowForceSource.Constants = { 0.0f, 1.0f, 0.0f };
		OverflowForceSource.ScalarPropertyIds = { "health" };
		OverflowForceSource.Instructions = {
			{ BlueprintOpcode::PushConstant, 1, 0, UUID(67, 10) },
			{ BlueprintOpcode::StoreScalar, 0, 0, UUID(67, 11) },
			{ BlueprintOpcode::LoadScalar, 0, 0, UUID(67, 12) },
			{ BlueprintOpcode::WriteScalarProperty, 0, 0, UUID(67, 13) },
			{ BlueprintOpcode::PushConstant, 0, 0, UUID(67, 1) },
			{ BlueprintOpcode::PushConstant, 1, 0, UUID(67, 2) },
			{ BlueprintOpcode::PushConstant, 2, 0, UUID(67, 3) },
			{ BlueprintOpcode::RequestApplyForceSelf, 0, 0, UUID(67, 4) },
			{ BlueprintOpcode::PushConstant, 0, 0, UUID(67, 5) },
			{ BlueprintOpcode::PushConstant, 1, 0, UUID(67, 6) },
			{ BlueprintOpcode::PushConstant, 2, 0, UUID(67, 7) },
			{ BlueprintOpcode::RequestApplyForceSelf, 0, 0, UUID(67, 8) },
			{ BlueprintOpcode::Return, 0, 0, UUID(67, 9) }
		};
		const auto VerifiedOverflowForce = VerifyBlueprintProgram(std::move(OverflowForceSource));
		PF_CHECK(Tests, VerifiedOverflowForce.has_value());
		if (!VerifiedOverflowForce) return;
		const auto OverflowForceProgram = std::make_shared<const VerifiedBlueprintProgram>(std::move(*VerifiedOverflowForce));
		constexpr UUID BulkForceTarget{ 0x5e00000000004000ull, 0x8000000000000001ull };
		constexpr UUID OverflowForceTarget{ 0x5e00000000004000ull, 0x8000000000000002ull };
		Scene OverflowForceScene;
		const Entity BulkForceEntity = AddPhysicsBody(OverflowForceScene, BulkForceTarget);
		const Entity OverflowForceEntity = AddPhysicsBody(OverflowForceScene, OverflowForceTarget, 4.0f);
		uint32_t OverflowPropertyTransactions = 0;
		SceneRuntimeServices OverflowForceServices;
		OverflowForceServices.ApplyBlueprintPropertyWrites = [&](const Entity& Target,
			std::span<const BlueprintScalarPropertyWrite> Writes)
		{
			if (Target == OverflowForceEntity && Writes.size() == 1 && Writes.front().Value == 1.0f)
				++OverflowPropertyTransactions;
			return true;
		};
		OverflowForceServices.ResolveBlueprintProgram = [&](const Entity& Target, BlueprintEventKind Event)
			-> std::optional<BlueprintProgramBinding>
		{
			if (Event != BlueprintEventKind::OnCreate) return std::nullopt;
			if (Target == BulkForceEntity) return BlueprintProgramBinding{ ProgramAsset, 9, BulkForceProgram };
			if (Target == OverflowForceEntity) return BlueprintProgramBinding{ ProgramAsset, 10, OverflowForceProgram };
			return std::nullopt;
		};
		SceneRuntime OverflowForceRuntime(*ProjectResult, ForceDescription, OverflowForceServices);
		PF_CHECK(Tests, OverflowForceRuntime.Start(OverflowForceScene).has_value());
		const auto OverflowForceFrame = OverflowForceRuntime.Advance(OverflowForceScene, Timestep(1.0 / 60.0));
		const auto BulkPosition = BulkForceEntity.GetTransform();
		const auto OverflowPosition = OverflowForceEntity.GetTransform();
		PF_CHECK(Tests, OverflowForceFrame && BulkPosition && BulkPosition->Translation.y > 0.0f && OverflowPosition &&
			OverflowPosition->Translation.y == 0.0f);
		const auto OverflowDiagnostics = OverflowForceRuntime.GetBlueprintDiagnostics();
		PF_CHECK(Tests, OverflowDiagnostics.size() == 1 && OverflowDiagnostics.front().Entity == OverflowForceTarget &&
			OverflowDiagnostics.front().Code == BlueprintProgramErrorCode::HostRejectedDeferredCall &&
			OverflowDiagnostics.front().SourceNode == UUID(67, 4));
		PF_CHECK(Tests, OverflowPropertyTransactions == 0);
		OverflowForceRuntime.Stop();

		constexpr UUID SpawnTarget{ 0x5f00000000004000ull, 0x8000000000000013ull };
		constexpr AssetID SpawnProgramAsset{ 0x5f00000000004000ull, 0x8000000000000014ull };
		BlueprintProgramSource SpawnSource;
		SpawnSource.Constants = { 3.0f, 4.0f, 5.0f };
		SpawnSource.Instructions = {
			{ BlueprintOpcode::PushConstant, 0, 0, UUID(68, 1) },
			{ BlueprintOpcode::PushConstant, 1, 0, UUID(68, 2) },
			{ BlueprintOpcode::PushConstant, 2, 0, UUID(68, 3) },
			{ BlueprintOpcode::RequestSpawnEntity, 0, 0, UUID(68, 4) },
			{ BlueprintOpcode::Return, 0, 0, UUID(68, 5) }
		};
		const auto VerifiedSpawn = VerifyBlueprintProgram(std::move(SpawnSource));
		PF_CHECK(Tests, VerifiedSpawn.has_value());
		if (!VerifiedSpawn) return;
		const auto SpawnProgram = std::make_shared<const VerifiedBlueprintProgram>(std::move(*VerifiedSpawn));
		Scene SpawnScene;
		const auto SpawnEntity = SpawnScene.CreateEntityWithUUID(SpawnTarget, "Blueprint spawn request owner");
		PF_CHECK(Tests, SpawnEntity.has_value());
		if (!SpawnEntity) return;
		Entity CurrentSpawnTarget = *SpawnEntity;
		SceneRuntimeServices SpawnServices;
		SpawnServices.ResolveBlueprintProgram = [&](const Entity& Target, BlueprintEventKind Event)
			-> std::optional<BlueprintProgramBinding>
		{
			if (Target == CurrentSpawnTarget && Event == BlueprintEventKind::OnCreate)
				return BlueprintProgramBinding{ SpawnProgramAsset, 1, SpawnProgram };
			return std::nullopt;
		};
		SceneRuntime SpawnRuntime(*ProjectResult, {}, SpawnServices);
		PF_CHECK(Tests, SpawnRuntime.Start(SpawnScene).has_value());
		PF_CHECK(Tests, SpawnScene.GetEntityCount() == 1);
		PF_CHECK(Tests, SpawnRuntime.Advance(SpawnScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, SpawnScene.GetEntityCount() == 2);
		for (const Entity& Spawned : SpawnScene.GetEntities())
		{
			if (Spawned == *SpawnEntity) continue;
			const auto Tag = Spawned.GetTag();
			const auto Transform = Spawned.GetTransform();
			PF_CHECK(Tests, Tag && Tag->Name == "Blueprint Spawned Entity" && Transform &&
				Transform->Translation == glm::vec3(3.0f, 4.0f, 5.0f));
		}
		SpawnRuntime.Stop();
		Scene CancelledSpawnScene;
		const auto CancelledSpawnEntity = CancelledSpawnScene.CreateEntityWithUUID(SpawnTarget, "Cancelled Blueprint spawn owner");
		PF_CHECK(Tests, CancelledSpawnEntity.has_value());
		if (!CancelledSpawnEntity) return;
		CurrentSpawnTarget = *CancelledSpawnEntity;
		SceneRuntime CancelledSpawnRuntime(*ProjectResult, {}, SpawnServices);
		PF_CHECK(Tests, CancelledSpawnRuntime.Start(CancelledSpawnScene).has_value());
		PF_CHECK(Tests, CancelledSpawnScene.GetEntityCount() == 1);
		CancelledSpawnRuntime.Stop();
		PF_CHECK(Tests, CancelledSpawnScene.GetEntityCount() == 1);

		Scene StaleSpawnScene;
		const auto StaleSpawnEntity = StaleSpawnScene.CreateEntityWithUUID(SpawnTarget, "Stale Blueprint spawn owner");
		PF_CHECK(Tests, StaleSpawnEntity.has_value());
		if (!StaleSpawnEntity) return;
		CurrentSpawnTarget = *StaleSpawnEntity;
		SceneRuntime StaleSpawnRuntime(*ProjectResult, {}, SpawnServices);
		PF_CHECK(Tests, StaleSpawnRuntime.Start(StaleSpawnScene).has_value());
		PF_CHECK(Tests, StaleSpawnScene.DestroyEntity(*StaleSpawnEntity).has_value());
		PF_CHECK(Tests, StaleSpawnRuntime.Advance(StaleSpawnScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, StaleSpawnScene.GetEntityCount() == 0);
		StaleSpawnRuntime.Stop();

		BlueprintProgramSource FullSpawnBatchSource;
		FullSpawnBatchSource.Constants = { 8.0f, 9.0f, 10.0f };
		for (size_t SpawnIndex = 0; SpawnIndex < BlueprintMaximumEntitySpawnsPerEvent; ++SpawnIndex)
		{
			const size_t Instruction = SpawnIndex * 4;
			FullSpawnBatchSource.Instructions.push_back({ BlueprintOpcode::PushConstant, 0, 0, UUID(74, Instruction + 1) });
			FullSpawnBatchSource.Instructions.push_back({ BlueprintOpcode::PushConstant, 1, 0, UUID(74, Instruction + 2) });
			FullSpawnBatchSource.Instructions.push_back({ BlueprintOpcode::PushConstant, 2, 0, UUID(74, Instruction + 3) });
			FullSpawnBatchSource.Instructions.push_back({ BlueprintOpcode::RequestSpawnEntity, 0, 0, UUID(74, Instruction + 4) });
		}
		FullSpawnBatchSource.Instructions.push_back({ BlueprintOpcode::Return, 0, 0, UUID(74, 257) });
		const auto VerifiedFullSpawnBatch = VerifyBlueprintProgram(std::move(FullSpawnBatchSource));
		PF_CHECK(Tests, VerifiedFullSpawnBatch.has_value());
		if (!VerifiedFullSpawnBatch) return;
		const auto FullSpawnBatchProgram = std::make_shared<const VerifiedBlueprintProgram>(std::move(*VerifiedFullSpawnBatch));
		constexpr AssetID SpawnBatchAsset{ 0x5f00000000004000ull, 0x8000000000000005ull };
		std::array<Entity, 5> SpawnBatchOwners;
		Scene SpawnBatchScene;
		for (size_t OwnerIndex = 0; OwnerIndex < SpawnBatchOwners.size(); ++OwnerIndex)
		{
			auto Owner = SpawnBatchScene.CreateEntityWithUUID(UUID(0x5f00000000004000ull, 0x8000000000000100ull + OwnerIndex),
				"Blueprint spawn budget owner");
			PF_CHECK(Tests, Owner.has_value());
			if (!Owner) return;
			SpawnBatchOwners[OwnerIndex] = *Owner;
		}
		SceneRuntimeServices SpawnBatchServices;
		SpawnBatchServices.ResolveBlueprintProgram = [&](const Entity& Target, BlueprintEventKind Event)
			-> std::optional<BlueprintProgramBinding>
		{
			if (Event != BlueprintEventKind::OnCreate ||
				std::find(SpawnBatchOwners.begin(), SpawnBatchOwners.end(), Target) == SpawnBatchOwners.end()) return std::nullopt;
			return BlueprintProgramBinding{ SpawnBatchAsset, 1, FullSpawnBatchProgram };
		};
		SceneRuntime SpawnBatchRuntime(*ProjectResult, {}, SpawnBatchServices);
		PF_CHECK(Tests, SpawnBatchRuntime.Start(SpawnBatchScene).has_value());
		PF_CHECK(Tests, SpawnBatchRuntime.Advance(SpawnBatchScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, SpawnBatchScene.GetEntityCount() == SpawnBatchOwners.size() + 4 * BlueprintMaximumEntitySpawnsPerEvent);
		const auto SpawnBatchDiagnostics = SpawnBatchRuntime.GetBlueprintDiagnostics();
		PF_CHECK(Tests, SpawnBatchDiagnostics.size() == 1 &&
			SpawnBatchDiagnostics.front().Entity == SpawnBatchOwners.back().GetUUID() &&
			SpawnBatchDiagnostics.front().Code == BlueprintProgramErrorCode::HostRejectedDeferredCall &&
			SpawnBatchDiagnostics.front().SourceNode == UUID(74, 4));
		SpawnBatchRuntime.Stop();

		constexpr UUID TimerTarget{ 0x5f00000000004000ull, 0x8000000000000003ull };
		constexpr AssetID TimerProgramAsset{ 0x5f00000000004000ull, 0x8000000000000004ull };
		BlueprintProgramSource ScheduleTimerSource;
		ScheduleTimerSource.Constants = { 0.05f };
		ScheduleTimerSource.ScalarPropertyIds = { "health" };
		ScheduleTimerSource.TimerNames = { "cooldown" };
		ScheduleTimerSource.Instructions = {
			{ BlueprintOpcode::RequestTimerSelf, 0, 0, UUID(69, 1) },
			{ BlueprintOpcode::Return, 0, 0, UUID(69, 2) }
		};
		const auto VerifiedScheduleTimer = VerifyBlueprintProgram(std::move(ScheduleTimerSource));
		PF_CHECK(Tests, VerifiedScheduleTimer.has_value());
		if (!VerifiedScheduleTimer) return;
		const auto ScheduleTimerProgram = std::make_shared<const VerifiedBlueprintProgram>(std::move(*VerifiedScheduleTimer));
		BlueprintProgramSource TimerCallbackSource;
		TimerCallbackSource.Constants = { 1.0f };
		TimerCallbackSource.ScalarPropertyIds = { "health" };
		TimerCallbackSource.TimerNames = { "cooldown" };
		TimerCallbackSource.Instructions = {
			{ BlueprintOpcode::JumpIfTimer, 0, 2, UUID(70, 1) },
			{ BlueprintOpcode::Return, 0, 0, UUID(70, 2) },
			{ BlueprintOpcode::PushConstant, 0, 0, UUID(70, 3) },
			{ BlueprintOpcode::WriteScalarProperty, 0, 0, UUID(70, 4) },
			{ BlueprintOpcode::Return, 0, 0, UUID(70, 5) }
		};
		const auto VerifiedTimerCallback = VerifyBlueprintProgram(std::move(TimerCallbackSource));
		PF_CHECK(Tests, VerifiedTimerCallback.has_value());
		if (!VerifiedTimerCallback) return;
		const auto TimerCallbackProgram = std::make_shared<const VerifiedBlueprintProgram>(std::move(*VerifiedTimerCallback));
		Scene TimerScene;
		const auto TimerEntity = TimerScene.CreateEntityWithUUID(TimerTarget, "Blueprint timer target");
		PF_CHECK(Tests, TimerEntity.has_value());
		if (!TimerEntity) return;
		uint64_t TimerCatalogGeneration = 1;
		uint32_t TimerPropertyTransactions = 0;
		SceneRuntimeServices TimerServices;
		TimerServices.GetBlueprintProgramGeneration = [&] { return TimerCatalogGeneration; };
		TimerServices.ResolveBlueprintProgram = [&](const Entity& Target, BlueprintEventKind Event)
			-> std::optional<BlueprintProgramBinding>
		{
			if (Target != *TimerEntity) return std::nullopt;
			if (Event == BlueprintEventKind::OnCreate)
				return BlueprintProgramBinding{ TimerProgramAsset, 1, ScheduleTimerProgram };
			if (Event == BlueprintEventKind::OnTimer)
				return BlueprintProgramBinding{ TimerProgramAsset, 1, TimerCallbackProgram };
			return std::nullopt;
		};
		TimerServices.ApplyBlueprintPropertyWrites = [&](const Entity& Target,
			std::span<const BlueprintScalarPropertyWrite> Writes)
		{
			if (Target == *TimerEntity && Writes.size() == 1 && Writes.front().PropertyId == "health" &&
				Writes.front().Value == 1.0f)
				++TimerPropertyTransactions;
			return true;
		};
		SceneRuntime TimerRuntime(*ProjectResult, {}, TimerServices);
		PF_CHECK(Tests, TimerRuntime.Start(TimerScene).has_value());
		PF_CHECK(Tests, TimerRuntime.Advance(TimerScene, Timestep(0.02)).has_value());
		PF_CHECK(Tests, TimerPropertyTransactions == 0);
		PF_CHECK(Tests, TimerRuntime.Advance(TimerScene, Timestep(0.04)).has_value());
		PF_CHECK(Tests, TimerPropertyTransactions == 1 && TimerRuntime.GetBlueprintDiagnostics().empty());
		TimerRuntime.Stop();

		Scene CancelledTimerScene;
		const auto CancelledTimerEntity = CancelledTimerScene.CreateEntityWithUUID(TimerTarget, "Cancelled timer target");
		PF_CHECK(Tests, CancelledTimerEntity.has_value());
		if (!CancelledTimerEntity) return;
		TimerServices.ResolveBlueprintProgram = [&](const Entity& Target, BlueprintEventKind Event)
			-> std::optional<BlueprintProgramBinding>
		{
			if (Target != *CancelledTimerEntity) return std::nullopt;
			if (Event == BlueprintEventKind::OnCreate)
				return BlueprintProgramBinding{ TimerProgramAsset, 1, ScheduleTimerProgram };
			if (Event == BlueprintEventKind::OnTimer)
				return BlueprintProgramBinding{ TimerProgramAsset, 1, TimerCallbackProgram };
			return std::nullopt;
		};
		SceneRuntime CancelledTimerRuntime(*ProjectResult, {}, TimerServices);
		PF_CHECK(Tests, CancelledTimerRuntime.Start(CancelledTimerScene).has_value());
		PF_CHECK(Tests, CancelledTimerRuntime.Advance(CancelledTimerScene, Timestep(0.01)).has_value());
		const uint32_t TransactionsBeforeGenerationChange = TimerPropertyTransactions;
		++TimerCatalogGeneration;
		PF_CHECK(Tests, CancelledTimerRuntime.Advance(CancelledTimerScene, Timestep(0.1)).has_value());
		PF_CHECK(Tests, TimerPropertyTransactions == TransactionsBeforeGenerationChange);
		CancelledTimerRuntime.Stop();

		Scene ReplacedTimerScene;
		const auto OriginalTimerEntity = ReplacedTimerScene.CreateEntityWithUUID(TimerTarget, "Original timer incarnation");
		PF_CHECK(Tests, OriginalTimerEntity.has_value());
		if (!OriginalTimerEntity) return;
		Entity CurrentTimerEntity = *OriginalTimerEntity;
		uint32_t ReplacementTimerTransactions = 0;
		TimerServices.ResolveBlueprintProgram = [&](const Entity& Target, BlueprintEventKind Event)
			-> std::optional<BlueprintProgramBinding>
		{
			if (Target != CurrentTimerEntity) return std::nullopt;
			if (Event == BlueprintEventKind::OnCreate)
				return BlueprintProgramBinding{ TimerProgramAsset, 1, ScheduleTimerProgram };
			if (Event == BlueprintEventKind::OnTimer)
				return BlueprintProgramBinding{ TimerProgramAsset, 1, TimerCallbackProgram };
			return std::nullopt;
		};
		TimerServices.ApplyBlueprintPropertyWrites = [&](const Entity& Target,
			std::span<const BlueprintScalarPropertyWrite> Writes)
		{
			if (Target == CurrentTimerEntity && Writes.size() == 1 && Writes.front().PropertyId == "health" &&
				Writes.front().Value == 1.0f)
				++ReplacementTimerTransactions;
			return true;
		};
		SceneRuntime ReplacedTimerRuntime(*ProjectResult, {}, TimerServices);
		PF_CHECK(Tests, ReplacedTimerRuntime.Start(ReplacedTimerScene).has_value());
		PF_CHECK(Tests, ReplacedTimerRuntime.Advance(ReplacedTimerScene, Timestep(0.01)).has_value());
		PF_CHECK(Tests, ReplacedTimerScene.DestroyEntity(CurrentTimerEntity).has_value());
		const auto ReplacementTimerEntity = ReplacedTimerScene.CreateEntityWithUUID(TimerTarget, "Replacement timer incarnation");
		PF_CHECK(Tests, ReplacementTimerEntity.has_value() && *ReplacementTimerEntity != CurrentTimerEntity);
		if (!ReplacementTimerEntity) return;
		CurrentTimerEntity = *ReplacementTimerEntity;
		PF_CHECK(Tests, ReplacedTimerRuntime.Advance(ReplacedTimerScene, Timestep(0.04)).has_value());
		PF_CHECK(Tests, ReplacementTimerTransactions == 0);
		PF_CHECK(Tests, ReplacedTimerRuntime.Advance(ReplacedTimerScene, Timestep(0.02)).has_value());
		PF_CHECK(Tests, ReplacementTimerTransactions == 1 && ReplacedTimerRuntime.GetBlueprintDiagnostics().empty());
		ReplacedTimerRuntime.Stop();

		const std::vector<std::byte> WaveBytes = MakeBlueprintTestWave();
		const auto AudioAsset = AssetOperations::CreateAssetFromBytes(ProjectResult->GetAssetRegistry(),
			ProjectResult->GetRootPath(), WaveBytes, "Assets/BlueprintAudio.wav");
		PF_CHECK(Tests, AudioAsset.has_value());
		if (!AudioAsset) return;
		constexpr UUID AudioTarget{ 0x5f00000000004000ull, 0x8000000000000001ull };
		Scene AudioControlScene;
		const auto AudioEntity = AudioControlScene.CreateEntityWithUUID(AudioTarget, "Blueprint audio control target");
		PF_CHECK(Tests, AudioEntity.has_value());
		if (!AudioEntity) return;
		PF_CHECK(Tests, AudioEntity->SetAudioSource(AudioSourceComponent{ AudioAsset->ID, 1.0f, true, true, false }).has_value());
		PF_CHECK(Tests, AudioEntity->SetAudioListener(AudioListenerComponent{}).has_value());
		BlueprintProgramSource AudioControlSource;
		AudioControlSource.Instructions = {
			{ BlueprintOpcode::RequestAudioControlSelf,
				static_cast<uint32_t>(BlueprintAudioControl::Pause), 0, UUID(68, 1) },
			{ BlueprintOpcode::Return, 0, 0, UUID(68, 2) }
		};
		const auto VerifiedAudioControl = VerifyBlueprintProgram(std::move(AudioControlSource));
		PF_CHECK(Tests, VerifiedAudioControl.has_value());
		if (!VerifiedAudioControl) return;
		const auto AudioControlProgram = std::make_shared<const VerifiedBlueprintProgram>(std::move(*VerifiedAudioControl));
		SceneRuntimeServices AudioControlServices;
		AudioControlServices.ResolveBlueprintProgram = [&](const Entity& Target, BlueprintEventKind Event)
			-> std::optional<BlueprintProgramBinding>
		{
			if (Target.GetUUID() != AudioTarget || Event != BlueprintEventKind::OnCreate) return std::nullopt;
			return BlueprintProgramBinding{ ProgramAsset, 11, AudioControlProgram };
		};
		SceneRuntime AudioControlRuntime(*ProjectResult, ForceDescription, AudioControlServices);
		PF_CHECK(Tests, AudioControlRuntime.Start(AudioControlScene).has_value());
		const auto AudioControlFrame = AudioControlRuntime.Advance(AudioControlScene, Timestep(1.0 / 60.0));
		PF_CHECK(Tests, AudioControlFrame.has_value());
		PF_CHECK(Tests, AudioControlRuntime.GetBlueprintDiagnostics().empty());
		AudioControlRuntime.Stop();
	}
}

void RunBlueprintSceneRuntimeTests(TestRunner& Tests)
{
	TestSceneRuntimeBlueprintEvents(Tests);
	TestSceneRuntimeBoundsGameplayActionFanout(Tests);
	TestSceneRuntimeBlueprintContactEvents(Tests);
	TestSceneRuntimeBlueprintDeferredStop(Tests);
}
