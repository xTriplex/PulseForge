#include "TestSupport.h"

#include "Input/GameplayInput.h"

#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace
{
	using namespace PulseForge;

	constexpr InputWindowID TestWindow = 41;

	InputControl Keyboard(InputKey Key)
	{
		return { .Device = InputDevice::Keyboard, .Code = static_cast<uint16_t>(Key) };
	}

	InputControl GamepadAxisControl(GamepadAxis Axis)
	{
		return { .Device = InputDevice::GamepadAxis, .Code = static_cast<uint16_t>(Axis) };
	}

	InputControl GamepadButtonControl(GamepadButton Button)
	{
		return { .Device = InputDevice::GamepadButton, .Code = static_cast<uint16_t>(Button) };
	}

	InputControl MouseButtonControl(MouseControl Button)
	{
		return { .Device = InputDevice::MouseButton, .Code = static_cast<uint16_t>(Button) };
	}

	ActionBinding ButtonBinding(std::string Id, InputControl Control, float X = 1.0f, float Y = 0.0f)
	{
		ActionBinding Binding;
		Binding.Id = std::move(Id);
		Binding.Kind = ActionBindingKind::Button;
		Binding.Primary = Control;
		Binding.ScaleX = X;
		Binding.ScaleY = Y;
		return Binding;
	}

	GameplayActionDefinition DigitalAction(std::string Id, InputControl Control, InputContext Context = InputContext::Gameplay)
	{
		GameplayActionDefinition Action;
		Action.Id = std::move(Id);
		Action.Type = ActionValueType::Digital;
		Action.Context = Context;
		Action.Bindings.push_back(ButtonBinding(Action.Id + ".primary", Control));
		return Action;
	}

	InputRoutingState GameplayRouting(InputWindowID Window = TestWindow)
	{
		InputRoutingState Routing;
		Routing.FocusedWindow = Window;
		Routing.Contexts[0] = {
			.Context = InputContext::Gameplay,
			.Consumes = InputDeviceMask::All,
			.OwnerWindow = Window,
			.Active = true
		};
		Routing.ContextCount = 1;
		return Routing;
	}

	InputEvent ButtonEvent(InputEventKind Kind, InputControl Control, InputWindowID Window = TestWindow, uint64_t Device = 0)
	{
		return { .Kind = Kind, .Control = Control, .Window = Window, .DeviceInstance = Device };
	}

	const ActionValue* Find(const SimulationActionSnapshot& Snapshot, std::string_view Id)
	{
		for (const auto& Entry : Snapshot.Actions)
		{
			if (Entry.Id == Id) return &Entry.Value;
		}
		return nullptr;
	}

	void TestActionMapValidationAndJson(TestRunner& Tests)
	{
		GameplayActionMap Map;
		Map.Actions.push_back(DigitalAction("Jump", Keyboard(InputKey::Space)));
		Map.Actions[0].Bindings[0].ModifierCount = 1;
		Map.Actions[0].Bindings[0].Modifiers[0] = Keyboard(InputKey::LeftShift);
		GameplayActionDefinition Move;
		Move.Id = "Move";
		Move.Type = ActionValueType::Axis2D;
		Move.Context = InputContext::Gameplay;
		Move.Bindings = {
			ButtonBinding("move.forward", Keyboard(InputKey::W), 0.0f, 1.0f),
			ButtonBinding("move.backward", Keyboard(InputKey::S), 0.0f, -1.0f),
			ButtonBinding("move.left", Keyboard(InputKey::A), -1.0f, 0.0f),
			ButtonBinding("move.right", Keyboard(InputKey::D), 1.0f, 0.0f)
		};
		Map.Actions.push_back(std::move(Move));

		const auto Valid = ValidateGameplayActionMap(Map);
		PF_CHECK(Tests, Valid.has_value());
		const auto Serialized = SerializeGameplayActionMap(Map);
		PF_CHECK(Tests, Serialized.has_value());
		if (!Serialized) return;
		const auto Parsed = ParseGameplayActionMap(*Serialized);
		PF_CHECK(Tests, Parsed.has_value());
		if (Parsed)
		{
			PF_CHECK(Tests, Parsed->Version == Map.Version);
			PF_CHECK(Tests, Parsed->Actions.size() == Map.Actions.size());
			PF_CHECK(Tests, Parsed->Actions[0].Id == "Jump");
			PF_CHECK(Tests, Parsed->Actions[0].Type == ActionValueType::Digital);
			PF_CHECK(Tests, Parsed->Actions[0].Bindings.size() == 1);
			PF_CHECK(Tests, Parsed->Actions[0].Bindings[0].Primary == Keyboard(InputKey::Space));
			PF_CHECK(Tests, Parsed->Actions[0].Bindings[0].Id == "Jump.primary");
			PF_CHECK(Tests, Parsed->Actions[0].Bindings[0].ModifierCount == 1);
			PF_CHECK(Tests, Parsed->Actions[0].Bindings[0].Modifiers[0] == Keyboard(InputKey::LeftShift));
			PF_CHECK(Tests, Parsed->Actions[1].Id == "Move");
			PF_CHECK(Tests, Parsed->Actions[1].Type == ActionValueType::Axis2D);
			PF_CHECK(Tests, Parsed->Actions[1].Bindings.size() == 4);
			PF_CHECK(Tests, Parsed->Actions[1].Bindings[0].Id == "move.forward");
			PF_CHECK(Tests, Parsed->Actions[1].Bindings[3].Id == "move.right");
			PF_CHECK(Tests, Parsed->Actions[1].Bindings[2].ScaleX == -1.0f);
		}

		PF_CHECK(Tests, !ParseInputControlName("PlatformSpecificScanCode").has_value());
		PF_CHECK(Tests, !ParseGameplayActionMap("{").has_value());
		const std::string OversizedDocument(1024 * 1024 + 1, ' ');
		PF_CHECK(Tests, !ParseGameplayActionMap(OversizedDocument).has_value());

		auto Duplicate = Map;
		Duplicate.Actions.push_back(Duplicate.Actions.front());
		PF_CHECK(Tests, !ValidateGameplayActionMap(Duplicate).has_value());

		auto InvalidBinding = Map;
		InvalidBinding.Actions[0].Bindings[0].ModifierCount = 5;
		PF_CHECK(Tests, !ValidateGameplayActionMap(InvalidBinding).has_value());

		auto InvalidNumber = Map;
		InvalidNumber.Actions[1].Bindings[0].DeadZone = std::numeric_limits<float>::quiet_NaN();
		PF_CHECK(Tests, !ValidateGameplayActionMap(InvalidNumber).has_value());
		InvalidNumber.Actions[1].Bindings[0].DeadZone = 0.96f;
		PF_CHECK(Tests, !ValidateGameplayActionMap(InvalidNumber).has_value());

		auto TooManyBindings = Map;
		TooManyBindings.Actions[1].Bindings.resize(65, ButtonBinding("move.extra", Keyboard(InputKey::A)));
		PF_CHECK(Tests, !ValidateGameplayActionMap(TooManyBindings).has_value());

		GameplayActionMap TooManyActions;
		TooManyActions.Actions.reserve(GameplayInputService::MaximumActions + 1);
		for (size_t Index = 0; Index <= GameplayInputService::MaximumActions; ++Index)
		{
			TooManyActions.Actions.push_back(DigitalAction("Action" + std::to_string(Index), Keyboard(InputKey::Space)));
		}
		PF_CHECK(Tests, !ValidateGameplayActionMap(TooManyActions).has_value());
		PF_CHECK(Tests, !SerializeGameplayActionMap(TooManyActions).has_value());
	}

	void TestButtonEdgesAndImmutableSnapshots(TestRunner& Tests)
	{
		GameplayInputService Service;
		GameplayActionMap Map;
		Map.Actions.push_back(DigitalAction("Jump", Keyboard(InputKey::Space)));
		PF_CHECK(Tests, Service.SetActionMap(std::move(Map)).has_value());
		auto Routing = GameplayRouting();

		PF_CHECK(Tests, Service.QueueEvent(ButtonEvent(InputEventKind::Pressed, Keyboard(InputKey::Space))));
		Service.PublishFrame(Routing);
		const auto* Pressed = Service.FindAction("Jump");
		PF_CHECK(Tests, Pressed && Pressed->Held && Pressed->Pressed && !Pressed->Released && !Pressed->Canceled);
		if (!Pressed) return;
		const ActionValue PublishedValue = *Pressed;
		PF_CHECK(Tests, Service.FindAction("Jump") && Service.FindAction("Jump")->Pressed);
		PF_CHECK(Tests, Service.FindAction("Jump") && Service.FindAction("Jump")->Pressed);
		PF_CHECK(Tests, PublishedValue.Pressed && PublishedValue.Held);

		Service.PublishFrame(Routing);
		const auto* Held = Service.FindAction("Jump");
		PF_CHECK(Tests, Held && Held->Held && !Held->Pressed && !Held->Released);

		PF_CHECK(Tests, Service.QueueEvent(ButtonEvent(InputEventKind::Released, Keyboard(InputKey::Space))));
		Service.PublishFrame(Routing);
		const auto* Released = Service.FindAction("Jump");
		PF_CHECK(Tests, Released && !Released->Held && !Released->Pressed && Released->Released);

		PF_CHECK(Tests, Service.QueueEvent(ButtonEvent(InputEventKind::Pressed, Keyboard(InputKey::Space))));
		PF_CHECK(Tests, Service.QueueEvent(ButtonEvent(InputEventKind::Released, Keyboard(InputKey::Space))));
		Service.PublishFrame(Routing);
		const auto* Tap = Service.FindAction("Jump");
		PF_CHECK(Tests, Tap && !Tap->Held && Tap->Pressed && Tap->Released);
		PF_CHECK(Tests, Service.FindAction("Jump") && Service.FindAction("Jump")->Pressed &&
			Service.FindAction("Jump")->Released);
	}

	void TestModifierChordEvaluation(TestRunner& Tests)
	{
		GameplayInputService Service;
		GameplayActionMap Map;
		GameplayActionDefinition Chord = DigitalAction("Chord", Keyboard(InputKey::Space));
		Chord.Bindings[0].ModifierCount = 1;
		Chord.Bindings[0].Modifiers[0] = Keyboard(InputKey::LeftShift);
		Map.Actions.push_back(std::move(Chord));
		PF_CHECK(Tests, Service.SetActionMap(std::move(Map)).has_value());
		auto Routing = GameplayRouting();

		PF_CHECK(Tests, Service.QueueEvent(ButtonEvent(InputEventKind::Pressed, Keyboard(InputKey::Space))));
		Service.PublishFrame(Routing);
		PF_CHECK(Tests, Service.FindAction("Chord") && !Service.FindAction("Chord")->Held);
		PF_CHECK(Tests, Service.QueueEvent(ButtonEvent(InputEventKind::Pressed, Keyboard(InputKey::LeftShift))));
		Service.PublishFrame(Routing);
		PF_CHECK(Tests, Service.FindAction("Chord") && Service.FindAction("Chord")->Held &&
			Service.FindAction("Chord")->Pressed);
		PF_CHECK(Tests, Service.QueueEvent(ButtonEvent(InputEventKind::Released, Keyboard(InputKey::LeftShift))));
		Service.PublishFrame(Routing);
		PF_CHECK(Tests, Service.FindAction("Chord") && !Service.FindAction("Chord")->Held &&
			Service.FindAction("Chord")->Released);
	}

	void TestBindingCompositionAndNormalization(TestRunner& Tests)
	{
		GameplayInputService Service;
		GameplayActionMap Map;

		GameplayActionDefinition Horizontal;
		Horizontal.Id = "Horizontal";
		Horizontal.Type = ActionValueType::Axis1D;
		Horizontal.Bindings = {
			ButtonBinding("horizontal.left", Keyboard(InputKey::A), -1.0f),
			ButtonBinding("horizontal.right", Keyboard(InputKey::D), 1.0f)
		};
		Map.Actions.push_back(std::move(Horizontal));

		GameplayActionDefinition Move;
		Move.Id = "Move";
		Move.Type = ActionValueType::Axis2D;
		Move.Bindings = {
			ButtonBinding("move.forward", Keyboard(InputKey::W), 0.0f, 1.0f),
			ButtonBinding("move.right", Keyboard(InputKey::D), 1.0f, 0.0f)
		};
		Map.Actions.push_back(std::move(Move));

		GameplayActionDefinition Stick;
		Stick.Id = "Stick";
		Stick.Type = ActionValueType::Axis2D;
		ActionBinding StickBinding;
		StickBinding.Id = "stick.left";
		StickBinding.Kind = ActionBindingKind::Stick;
		StickBinding.Primary = GamepadAxisControl(GamepadAxis::LeftX);
		StickBinding.Secondary = GamepadAxisControl(GamepadAxis::LeftY);
		StickBinding.ScaleY = 1.0f;
		StickBinding.DeadZone = 0.2f;
		Stick.Bindings.push_back(StickBinding);
		Map.Actions.push_back(std::move(Stick));

		GameplayActionDefinition Trigger;
		Trigger.Id = "Trigger";
		Trigger.Type = ActionValueType::Axis1D;
		ActionBinding TriggerBinding;
		TriggerBinding.Id = "trigger.left";
		TriggerBinding.Kind = ActionBindingKind::Axis;
		TriggerBinding.Primary = GamepadAxisControl(GamepadAxis::LeftTrigger);
		Trigger.Bindings.push_back(TriggerBinding);
		Map.Actions.push_back(std::move(Trigger));
		const auto Serialized = SerializeGameplayActionMap(Map);
		PF_CHECK(Tests, Serialized.has_value());
		if (Serialized)
		{
			const auto Parsed = ParseGameplayActionMap(*Serialized);
			PF_CHECK(Tests, Parsed.has_value());
			if (Parsed)
			{
				PF_CHECK(Tests, Parsed->Actions[2].Bindings[0].Id == "stick.left");
				PF_CHECK(Tests, Parsed->Actions[3].Bindings[0].Id == "trigger.left");
			}
		}

		PF_CHECK(Tests, Service.SetActionMap(std::move(Map)).has_value());
		auto Routing = GameplayRouting();
		PF_CHECK(Tests, Service.QueueEvent(ButtonEvent(InputEventKind::Pressed, Keyboard(InputKey::A))));
		PF_CHECK(Tests, Service.QueueEvent(ButtonEvent(InputEventKind::Pressed, Keyboard(InputKey::D))));
		PF_CHECK(Tests, Service.QueueEvent(ButtonEvent(InputEventKind::Pressed, Keyboard(InputKey::W))));
		Service.PublishFrame(Routing);
		const auto* HorizontalValue = Service.FindAction("Horizontal");
		const auto* MoveValue = Service.FindAction("Move");
		PF_CHECK(Tests, HorizontalValue && std::abs(HorizontalValue->X) < 0.0001f);
		PF_CHECK(Tests, MoveValue && MoveValue->Held);
		if (MoveValue)
		{
			PF_CHECK(Tests, std::abs(std::sqrt(MoveValue->X * MoveValue->X + MoveValue->Y * MoveValue->Y) - 1.0f) < 0.0001f);
			PF_CHECK(Tests, MoveValue->X > 0.0f && MoveValue->Y > 0.0f);
		}

		Service.Reset();
		PF_CHECK(Tests, Service.QueueEvent({
			.Kind = InputEventKind::Value,
			.Control = GamepadAxisControl(GamepadAxis::LeftX),
			.Window = TestWindow,
			.DeviceInstance = 7,
			.X = 0.1f
		}));
		PF_CHECK(Tests, Service.QueueEvent({
			.Kind = InputEventKind::Value,
			.Control = GamepadAxisControl(GamepadAxis::LeftY),
			.Window = TestWindow,
			.DeviceInstance = 7,
			.X = 0.1f
		}));
		PF_CHECK(Tests, Service.QueueEvent({
			.Kind = InputEventKind::Value,
			.Control = GamepadAxisControl(GamepadAxis::LeftTrigger),
			.Window = TestWindow,
			.DeviceInstance = 7,
			.X = 1.7f
		}));
		Service.PublishFrame(Routing);
		const auto* InsideDeadZone = Service.FindAction("Stick");
		const auto* ClampedTrigger = Service.FindAction("Trigger");
		PF_CHECK(Tests, InsideDeadZone && std::abs(InsideDeadZone->X) < 0.0001f && std::abs(InsideDeadZone->Y) < 0.0001f);
		PF_CHECK(Tests, ClampedTrigger && ClampedTrigger->X >= 0.0f && ClampedTrigger->X <= 1.0f);

		Service.Reset();
		PF_CHECK(Tests, !Service.QueueEvent({
			.Kind = InputEventKind::Value,
			.Control = GamepadAxisControl(GamepadAxis::LeftX),
			.Window = TestWindow,
			.DeviceInstance = 7,
			.X = std::numeric_limits<float>::quiet_NaN()
		}));
		PF_CHECK(Tests, Service.QueueEvent({
			.Kind = InputEventKind::Value,
			.Control = GamepadAxisControl(GamepadAxis::LeftY),
			.Window = TestWindow,
			.DeviceInstance = 7,
			.X = 0.8f
		}));
		PF_CHECK(Tests, Service.QueueEvent({
			.Kind = InputEventKind::Value,
			.Control = GamepadAxisControl(GamepadAxis::LeftTrigger),
			.Window = TestWindow,
			.DeviceInstance = 7,
			.X = -0.5f
		}));
		Service.PublishFrame(Routing);
		const auto* InvalidStick = Service.FindAction("Stick");
		const auto* ClampedNegativeTrigger = Service.FindAction("Trigger");
		PF_CHECK(Tests, InvalidStick && std::isfinite(InvalidStick->X) && std::isfinite(InvalidStick->Y));
		PF_CHECK(Tests, ClampedNegativeTrigger && ClampedNegativeTrigger->X == 0.0f);

		Service.Reset();
		for (InputControl Control : { GamepadAxisControl(GamepadAxis::LeftX), GamepadAxisControl(GamepadAxis::LeftY) })
			PF_CHECK(Tests, Service.QueueEvent({ .Kind = InputEventKind::Value, .Control = Control,
				.Window = TestWindow, .DeviceInstance = 7, .X = 0.0f }));
		Service.PublishFrame(Routing);
		PF_CHECK(Tests, Service.QueueEvent({
			.Kind = InputEventKind::Value,
			.Control = GamepadAxisControl(GamepadAxis::LeftX),
			.Window = TestWindow,
			.DeviceInstance = 7,
			.X = 0.6f
		}));
		PF_CHECK(Tests, Service.QueueEvent({
			.Kind = InputEventKind::Value,
			.Control = GamepadAxisControl(GamepadAxis::LeftY),
			.Window = TestWindow,
			.DeviceInstance = 7,
			.X = 0.8f
		}));
		Service.PublishFrame(Routing);
		const auto* OutsideDeadZone = Service.FindAction("Stick");
		PF_CHECK(Tests, OutsideDeadZone && OutsideDeadZone->Held);
		if (OutsideDeadZone)
		{
			PF_CHECK(Tests, std::isfinite(OutsideDeadZone->X) && std::isfinite(OutsideDeadZone->Y));
			PF_CHECK(Tests, std::abs(std::sqrt(OutsideDeadZone->X * OutsideDeadZone->X +
				OutsideDeadZone->Y * OutsideDeadZone->Y) - 1.0f) < 0.0001f);
		}
	}

	void TestContextPriorityAndWindowOwnership(TestRunner& Tests)
	{
		GameplayInputService Service;
		GameplayActionMap Map;
		Map.Actions.push_back(DigitalAction("GameplayJump", Keyboard(InputKey::Space), InputContext::Gameplay));
		Map.Actions.push_back(DigitalAction("EditorCommand", Keyboard(InputKey::Space), InputContext::EditorShortcuts));
		Map.Actions.push_back(DigitalAction("GameplayClick", MouseButtonControl(MouseControl::Left), InputContext::Gameplay));
		PF_CHECK(Tests, Service.SetActionMap(std::move(Map)).has_value());

		InputRoutingState Routing = GameplayRouting();
		Routing.Contexts[1] = {
			.Context = InputContext::EditorShortcuts,
			.Consumes = InputDeviceMask::Keyboard,
			.OwnerWindow = TestWindow,
			.Active = true
		};
		Routing.ContextCount = 2;
		PF_CHECK(Tests, Service.QueueEvent(ButtonEvent(InputEventKind::Pressed, Keyboard(InputKey::Space))));
		PF_CHECK(Tests, Service.QueueEvent(ButtonEvent(InputEventKind::Pressed, MouseButtonControl(MouseControl::Left))));
		Service.PublishFrame(Routing);
		const auto* GameplayAction = Service.FindAction("GameplayJump");
		const auto* ShortcutAction = Service.FindAction("EditorCommand");
		PF_CHECK(Tests, GameplayAction && !GameplayAction->Held);
		PF_CHECK(Tests, ShortcutAction && ShortcutAction->Held && ShortcutAction->Pressed);
		PF_CHECK(Tests, Service.FindAction("GameplayClick") && Service.FindAction("GameplayClick")->Held);

		Service.Reset();
		Routing = GameplayRouting();
		Routing.FocusedWindow = TestWindow;
		Routing.Contexts[0].OwnerWindow = TestWindow + 1;
		PF_CHECK(Tests, Service.QueueEvent(ButtonEvent(InputEventKind::Pressed, Keyboard(InputKey::Space), TestWindow)));
		Service.PublishFrame(Routing);
		PF_CHECK(Tests, Service.FindAction("GameplayJump") && !Service.FindAction("GameplayJump")->Held);
		Routing.Contexts[0].OwnerWindow = TestWindow;
		PF_CHECK(Tests, Service.QueueEvent(ButtonEvent(InputEventKind::Released, Keyboard(InputKey::Space), TestWindow)));
		PF_CHECK(Tests, Service.QueueEvent(ButtonEvent(InputEventKind::Pressed, Keyboard(InputKey::Space), TestWindow)));
		Service.PublishFrame(Routing);
		PF_CHECK(Tests, Service.FindAction("GameplayJump") && Service.FindAction("GameplayJump")->Held);

		Service.Reset();
		Routing = GameplayRouting();
		Routing.Contexts[1] = {
			.Context = InputContext::TextEntry,
			.Consumes = InputDeviceMask::Keyboard,
			.OwnerWindow = TestWindow,
			.Active = true
		};
		Routing.ContextCount = 2;
		PF_CHECK(Tests, Service.QueueEvent(ButtonEvent(InputEventKind::Released, Keyboard(InputKey::Space))));
		PF_CHECK(Tests, Service.QueueEvent(ButtonEvent(InputEventKind::Released, MouseButtonControl(MouseControl::Left))));
		Service.PublishFrame(Routing);
		PF_CHECK(Tests, Service.QueueEvent(ButtonEvent(InputEventKind::Pressed, Keyboard(InputKey::Space))));
		PF_CHECK(Tests, Service.QueueEvent(ButtonEvent(InputEventKind::Pressed, MouseButtonControl(MouseControl::Left))));
		Service.PublishFrame(Routing);
		PF_CHECK(Tests, Service.FindAction("GameplayJump") && !Service.FindAction("GameplayJump")->Held);
		PF_CHECK(Tests, Service.FindAction("GameplayClick") && Service.FindAction("GameplayClick")->Held);
	}

	void TestDetachedViewportOwnerLifecycle(TestRunner& Tests)
	{
		GameplayInputService Service;
		GameplayActionMap Map;
		Map.Actions.push_back(DigitalAction("ViewportJump", Keyboard(InputKey::Space)));
		PF_CHECK(Tests, Service.SetActionMap(std::move(Map)).has_value());

		constexpr InputWindowID DetachedViewport = TestWindow + 17;
		InputRoutingState Routing = GameplayRouting();
		Routing.FocusedWindow = DetachedViewport;
		Routing.Contexts[0].OwnerWindow = DetachedViewport;
		PF_CHECK(Tests, Service.QueueEvent(ButtonEvent(InputEventKind::Pressed, Keyboard(InputKey::Space), DetachedViewport)));
		Service.PublishFrame(Routing);
		const auto* DetachedAction = Service.FindAction("ViewportJump");
		PF_CHECK(Tests, DetachedAction && DetachedAction->Held && DetachedAction->Pressed);

		PF_CHECK(Tests, Service.QueueEvent({ .Kind = InputEventKind::WindowDestroyed, .Window = DetachedViewport }));
		Service.PublishFrame(Routing);
		const auto* DestroyedAction = Service.FindAction("ViewportJump");
		PF_CHECK(Tests, DestroyedAction && !DestroyedAction->Held && DestroyedAction->Released && DestroyedAction->Canceled);

		Routing = GameplayRouting();
		PF_CHECK(Tests, Service.QueueEvent(ButtonEvent(InputEventKind::Pressed, Keyboard(InputKey::Space), DetachedViewport)));
		Service.PublishFrame(Routing);
		const auto* MainAction = Service.FindAction("ViewportJump");
		PF_CHECK(Tests, MainAction && !MainAction->Held && !MainAction->Pressed);
		PF_CHECK(Tests, Service.QueueEvent(ButtonEvent(InputEventKind::Released, Keyboard(InputKey::Space), DetachedViewport)));
		PF_CHECK(Tests, Service.QueueEvent(ButtonEvent(InputEventKind::Released, Keyboard(InputKey::Space), TestWindow)));
		PF_CHECK(Tests, Service.QueueEvent(ButtonEvent(InputEventKind::Pressed, Keyboard(InputKey::Space), TestWindow)));
		Service.PublishFrame(Routing);
		const auto* RearmedAction = Service.FindAction("ViewportJump");
		PF_CHECK(Tests, RearmedAction && RearmedAction->Held && RearmedAction->Pressed);
	}

	void TestCancellationQuarantineAndFixedStepEdges(TestRunner& Tests)
	{
		GameplayInputService Service;
		GameplayActionMap Map;
		Map.Actions.push_back(DigitalAction("Jump", Keyboard(InputKey::Space)));
		Map.Actions.push_back(DigitalAction("Fire", MouseButtonControl(MouseControl::Left)));
		PF_CHECK(Tests, Service.SetActionMap(std::move(Map)).has_value());
		auto Routing = GameplayRouting();

		PF_CHECK(Tests, Service.QueueEvent(ButtonEvent(InputEventKind::Pressed, Keyboard(InputKey::Space))));
		Service.PublishFrame(Routing);
		PF_CHECK(Tests, Service.FindAction("Jump") && Service.FindAction("Jump")->Pressed);

		InputEvent FocusLost{ .Kind = InputEventKind::FocusLost, .Window = TestWindow };
		PF_CHECK(Tests, Service.QueueEvent(FocusLost));
		Service.PublishFrame(Routing);
		const auto* FocusCanceled = Service.FindAction("Jump");
		PF_CHECK(Tests, FocusCanceled && !FocusCanceled->Held && FocusCanceled->Released && FocusCanceled->Canceled);

		PF_CHECK(Tests, Service.QueueEvent(ButtonEvent(InputEventKind::Pressed, Keyboard(InputKey::Space))));
		Service.PublishFrame(Routing);
		const auto* Quarantined = Service.FindAction("Jump");
		PF_CHECK(Tests, Quarantined && !Quarantined->Held && !Quarantined->Pressed);
		PF_CHECK(Tests, Service.QueueEvent(ButtonEvent(InputEventKind::Released, Keyboard(InputKey::Space))));
		Service.PublishFrame(Routing);
		PF_CHECK(Tests, Service.QueueEvent(ButtonEvent(InputEventKind::Pressed, Keyboard(InputKey::Space))));
		Service.PublishFrame(Routing);
		const auto* Rearmed = Service.FindAction("Jump");
		PF_CHECK(Tests, Rearmed && Rearmed->Held && Rearmed->Pressed);

		PF_CHECK(Tests, Service.QueueEvent(ButtonEvent(InputEventKind::Pressed, MouseButtonControl(MouseControl::Left))));
		Service.PublishFrame(Routing);
		PF_CHECK(Tests, Service.FindAction("Fire") && Service.FindAction("Fire")->Held);
		PF_CHECK(Tests, Service.QueueEvent({ .Kind = InputEventKind::CaptureCancelled, .Window = TestWindow }));
		Service.PublishFrame(Routing);
		const auto* CaptureCanceled = Service.FindAction("Fire");
		PF_CHECK(Tests, CaptureCanceled && !CaptureCanceled->Held && CaptureCanceled->Released && CaptureCanceled->Canceled);

		Service.Reset();
		PF_CHECK(Tests, Service.QueueEvent(ButtonEvent(InputEventKind::Released, Keyboard(InputKey::Space))));
		Service.PublishFrame(Routing);
		PF_CHECK(Tests, Service.QueueEvent(ButtonEvent(InputEventKind::Pressed, Keyboard(InputKey::Space))));
		Service.PublishFrame(Routing);
		Service.PublishFrame(Routing); // No simulation step: the edge must remain pending.
		const auto ZeroStep = Service.BeginSimulationStep();
		const auto* ZeroStepJump = Find(ZeroStep, "Jump");
		PF_CHECK(Tests, ZeroStepJump && ZeroStepJump->Pressed);
		const auto SecondStep = Service.BeginSimulationStep();
		const auto* SecondStepJump = Find(SecondStep, "Jump");
		PF_CHECK(Tests, SecondStepJump && !SecondStepJump->Pressed);

		Service.Reset();
		PF_CHECK(Tests, Service.QueueEvent(ButtonEvent(InputEventKind::Released, Keyboard(InputKey::Space))));
		Service.PublishFrame(Routing);
		PF_CHECK(Tests, Service.QueueEvent(ButtonEvent(InputEventKind::Pressed, Keyboard(InputKey::Space))));
		Service.PublishFrame(Routing);
		const auto FirstStep = Service.BeginSimulationStep();
		PF_CHECK(Tests, Find(FirstStep, "Jump") && Find(FirstStep, "Jump")->Pressed);
		const auto AdditionalStep = Service.BeginSimulationStep();
		PF_CHECK(Tests, Find(AdditionalStep, "Jump") && !Find(AdditionalStep, "Jump")->Pressed);
		PF_CHECK(Tests, Find(AdditionalStep, "Jump") && Find(AdditionalStep, "Jump")->Held);
	}

	void TestGamepadDisconnectReconnectQuarantine(TestRunner& Tests)
	{
		GameplayInputService Service;
		GameplayActionMap Map;
		Map.Actions.push_back(DigitalAction("Fire", GamepadButtonControl(GamepadButton::A)));
		PF_CHECK(Tests, Service.SetActionMap(std::move(Map)).has_value());
		const auto Routing = GameplayRouting();
		const InputControl Button = GamepadButtonControl(GamepadButton::A);

		PF_CHECK(Tests, Service.QueueEvent({
			.Kind = InputEventKind::DeviceConnected,
			.Control = Button,
			.DeviceInstance = 17
		}));
		PF_CHECK(Tests, Service.QueueEvent(ButtonEvent(InputEventKind::Pressed, Button, 0, 17)));
		Service.PublishFrame(Routing);
		const auto* HeldAtConnect = Service.FindAction("Fire");
		PF_CHECK(Tests, HeldAtConnect && !HeldAtConnect->Held && !HeldAtConnect->Pressed);

		Service.PublishFrame(Routing);
		const auto* StillHeldAtConnect = Service.FindAction("Fire");
		PF_CHECK(Tests, StillHeldAtConnect && !StillHeldAtConnect->Held && !StillHeldAtConnect->Pressed);

		PF_CHECK(Tests, Service.QueueEvent(ButtonEvent(InputEventKind::Released, Button, 0, 17)));
		Service.PublishFrame(Routing);
		PF_CHECK(Tests, Service.QueueEvent(ButtonEvent(InputEventKind::Pressed, Button, 0, 17)));
		Service.PublishFrame(Routing);
		const auto* FreshPress = Service.FindAction("Fire");
		PF_CHECK(Tests, FreshPress && FreshPress->Held && FreshPress->Pressed);

		PF_CHECK(Tests, Service.QueueEvent({
			.Kind = InputEventKind::DeviceDisconnected,
			.Control = Button,
			.DeviceInstance = 17
		}));
		Service.PublishFrame(Routing);
		const auto* Disconnected = Service.FindAction("Fire");
		PF_CHECK(Tests, Disconnected && !Disconnected->Held && Disconnected->Released && Disconnected->Canceled);

		PF_CHECK(Tests, Service.QueueEvent({
			.Kind = InputEventKind::DeviceConnected,
			.Control = Button,
			.DeviceInstance = 18
		}));
		PF_CHECK(Tests, Service.QueueEvent(ButtonEvent(InputEventKind::Pressed, Button, 0, 18)));
		Service.PublishFrame(Routing);
		const auto* HeldAfterReconnect = Service.FindAction("Fire");
		PF_CHECK(Tests, HeldAfterReconnect && !HeldAfterReconnect->Held && !HeldAfterReconnect->Pressed);

		PF_CHECK(Tests, Service.QueueEvent(ButtonEvent(InputEventKind::Released, Button, 0, 18)));
		Service.PublishFrame(Routing);
		PF_CHECK(Tests, Service.QueueEvent(ButtonEvent(InputEventKind::Pressed, Button, 0, 18)));
		Service.PublishFrame(Routing);
		const auto* ReconnectedFreshPress = Service.FindAction("Fire");
		PF_CHECK(Tests, ReconnectedFreshPress && ReconnectedFreshPress->Held && ReconnectedFreshPress->Pressed);
	}

	void TestOverflowFailsSafe(TestRunner& Tests)
	{
		GameplayInputService Service;
		GameplayActionMap Map;
		Map.Actions.push_back(DigitalAction("Jump", Keyboard(InputKey::Space)));
		PF_CHECK(Tests, Service.SetActionMap(std::move(Map)).has_value());
		auto Routing = GameplayRouting();
		PF_CHECK(Tests, Service.QueueEvent(ButtonEvent(InputEventKind::Pressed, Keyboard(InputKey::Space))));
		Service.PublishFrame(Routing);
		PF_CHECK(Tests, Service.FindAction("Jump") && Service.FindAction("Jump")->Held);

		for (size_t Index = 0; Index < GameplayInputService::MaximumQueuedEvents; ++Index)
		{
			const InputEvent Motion{
				.Kind = InputEventKind::PointerMoved,
				.Control = { .Device = InputDevice::MouseMotion, .Code = static_cast<uint16_t>(MouseControl::MotionX) },
				.Window = TestWindow,
				.X = static_cast<float>(Index),
				.Y = 0.0f
			};
			PF_CHECK(Tests, Service.QueueEvent(Motion));
		}
		const InputEvent OverflowMotion{
			.Kind = InputEventKind::PointerMoved,
			.Control = { .Device = InputDevice::MouseMotion, .Code = static_cast<uint16_t>(MouseControl::MotionX) },
			.Window = TestWindow,
			.X = 0.0f,
			.Y = 0.0f
		};
		PF_CHECK(Tests, !Service.QueueEvent(OverflowMotion));
		PF_CHECK(Tests, Service.DidOverflowEvents());
		Service.PublishFrame(Routing);
		const auto* AfterOverflow = Service.FindAction("Jump");
		PF_CHECK(Tests, AfterOverflow && !AfterOverflow->Held && AfterOverflow->Canceled);
	}

	void TestRejectedMapReplacementRetainsUsableActions(TestRunner& Tests)
	{
		GameplayInputService Service;
		GameplayActionMap Map;
		Map.Actions.push_back(DigitalAction("Jump", Keyboard(InputKey::Space)));
		PF_CHECK(Tests, Service.SetActionMap(std::move(Map)).has_value());
		auto Routing = GameplayRouting();
		PF_CHECK(Tests, Service.QueueEvent(ButtonEvent(InputEventKind::Pressed, Keyboard(InputKey::Space))));
		Service.PublishFrame(Routing);
		PF_CHECK(Tests, Service.FindAction("Jump") && Service.FindAction("Jump")->Held);

		GameplayActionMap InvalidReplacement;
		InvalidReplacement.Actions.push_back(DigitalAction("Jump", Keyboard(InputKey::Enter)));
		InvalidReplacement.Actions.push_back(DigitalAction("Jump", Keyboard(InputKey::Escape)));
		PF_CHECK(Tests, !Service.SetActionMap(std::move(InvalidReplacement)).has_value());
		PF_CHECK(Tests, Service.FindAction("Jump") && Service.FindAction("Jump")->Held);
		PF_CHECK(Tests, Service.QueueEvent(ButtonEvent(InputEventKind::Released, Keyboard(InputKey::Space))));
		Service.PublishFrame(Routing);
		PF_CHECK(Tests, Service.FindAction("Jump") && Service.FindAction("Jump")->Released &&
			!Service.FindAction("Jump")->Held);
	}
} // namespace

void RunGameplayInputTests(TestRunner& Tests)
{
	TestActionMapValidationAndJson(Tests);
	TestButtonEdgesAndImmutableSnapshots(Tests);
	TestModifierChordEvaluation(Tests);
	TestBindingCompositionAndNormalization(Tests);
	TestContextPriorityAndWindowOwnership(Tests);
		TestDetachedViewportOwnerLifecycle(Tests);
	TestCancellationQuarantineAndFixedStepEdges(Tests);
	TestGamepadDisconnectReconnectQuarantine(Tests);
	TestOverflowFailsSafe(Tests);
	TestRejectedMapReplacementRetainsUsableActions(Tests);
}
