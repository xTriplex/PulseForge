#include "TestEngineSupport.h"

#include "Assets/AssetOperations.h"
#include "Scene/Components/RuntimeUICanvasComponent.h"
#include "Scene/Components/ScriptComponent.h"
#include "UI/RuntimeUI.h"

#include <algorithm>
#include <array>
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
		for (uint64_t Index = 0; Index < 17; ++Index)
		{
			const UUID EntityId{ 0x5300000000004000ull, 0x8000000000000001ull + Index };
			PF_CHECK(Tests, TestScene.CreateEntityWithUUID(EntityId, "Action target").has_value());
		}
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
		Services.ResolveBlueprintProgram = [&](const Entity&, BlueprintEventKind Event)
			-> std::optional<BlueprintProgramBinding>
		{
			if (Event != BlueprintEventKind::OnActionPressed) return std::nullopt;
			return BlueprintProgramBinding{ ProgramAsset, 1, Program };
		};
		uint32_t PropertyWrites = 0;
		Services.ApplyBlueprintPropertyWrites = [&](const Entity& Target, std::span<const BlueprintScalarPropertyWrite> Writes)
		{
			if (!Target.IsValid() || Writes.size() != 1 || Writes[0].PropertyId != "hit" || Writes[0].Value != 1.0f)
				return false;
			++PropertyWrites;
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
		const auto Diagnostics = Runtime.GetBlueprintDiagnostics();
		PF_CHECK(Tests, std::any_of(Diagnostics.begin(), Diagnostics.end(), [](const BlueprintRuntimeDiagnostic& Diagnostic)
		{
			return Diagnostic.Event == BlueprintEventKind::OnActionPressed &&
				Diagnostic.Code == BlueprintProgramErrorCode::EventBudgetExceeded;
		}));
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
	}
}

void RunBlueprintSceneRuntimeTests(TestRunner& Tests)
{
	TestSceneRuntimeBlueprintEvents(Tests);
	TestSceneRuntimeBoundsGameplayActionFanout(Tests);
	TestSceneRuntimeBlueprintContactEvents(Tests);
	TestSceneRuntimeBlueprintDeferredStop(Tests);
}
