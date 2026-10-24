#include "TestSupport.h"

#include "Input/GameplayInput.h"
#include "Input/GamepadStateAdapter.h"

#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace
{
	using namespace PulseForge;
	constexpr InputWindowID TestWindow = 41;

	InputControl Axis(GamepadAxis Value) { return { InputDevice::GamepadAxis, static_cast<uint16_t>(Value) }; }
	InputControl Button(GamepadButton Value) { return { InputDevice::GamepadButton, static_cast<uint16_t>(Value) }; }
	InputControl Motion(MouseControl Value) { return { InputDevice::MouseMotion, static_cast<uint16_t>(Value) }; }
	InputControl Wheel(MouseControl Value) { return { InputDevice::MouseWheel, static_cast<uint16_t>(Value) }; }
	InputRoutingState Routing()
	{
		InputRoutingState Result;
		Result.FocusedWindow = TestWindow;
		Result.Contexts[0] = { .Context = InputContext::Gameplay, .Consumes = InputDeviceMask::All,
			.OwnerWindow = TestWindow, .Active = true };
		Result.ContextCount = 1;
		return Result;
	}
	ActionBinding AnalogBinding(std::string Id, InputControl Control, float DeadZone = 0.0f)
	{
		ActionBinding Result;
		Result.Id = std::move(Id);
		Result.Kind = ActionBindingKind::Axis;
		Result.Primary = Control;
		Result.DeadZone = DeadZone;
		return Result;
	}
	bool Queue(GameplayInputService& Input, const GamepadStateEvents& Events)
	{
		for (const InputEvent& Event : Events.View()) if (!Input.QueueEvent(Event)) return false;
		return true;
	}
	GamepadStateSample NeutralPad()
	{
		GamepadStateSample Result;
		Result.Axes[static_cast<size_t>(GamepadAxis::LeftTrigger)] = -1.0f;
		Result.Axes[static_cast<size_t>(GamepadAxis::RightTrigger)] = -1.0f;
		return Result;
	}

	void TestGamepadSampleNormalization(TestRunner& Tests)
	{
		GamepadStateAdapter Adapter;
		GamepadStateSample Sample = NeutralPad();
		Sample.Axes[static_cast<size_t>(GamepadAxis::RightX)] = 1.5f;
		Sample.Axes[static_cast<size_t>(GamepadAxis::LeftTrigger)] = std::numeric_limits<float>::quiet_NaN();
		const GamepadStateEvents Events = Adapter.Update(0, true, true, &Sample);
		PF_CHECK(Tests, Events.Count == GamepadStateSample::AxisCount + 1);
		PF_CHECK(Tests, Events.Values[1 + static_cast<size_t>(GamepadAxis::RightX)].X == 1.0f);
		PF_CHECK(Tests, Events.Values[1 + static_cast<size_t>(GamepadAxis::LeftTrigger)].X == 0.0f);
		PF_CHECK(Tests, Events.Values[1 + static_cast<size_t>(GamepadAxis::RightTrigger)].X == 0.0f);
	}

	void TestInitialGamepadSampleAndReconnect(TestRunner& Tests)
	{
		GamepadStateAdapter Adapter;
		GameplayInputService Input;
		GameplayActionMap Map;
		GameplayActionDefinition Move{ .Id = "Move", .Type = ActionValueType::Axis2D };
		ActionBinding Stick;
		Stick.Id = "Move.stick";
		Stick.Kind = ActionBindingKind::Stick;
		Stick.Primary = Axis(GamepadAxis::LeftX);
		Stick.Secondary = Axis(GamepadAxis::LeftY);
		Stick.ScaleX = 1.0f;
		Stick.ScaleY = 1.0f;
		Stick.DeadZone = 0.15f;
		Move.Bindings.push_back(Stick);
		GameplayActionDefinition Jump{ .Id = "Jump", .Type = ActionValueType::Digital };
		ActionBinding A;
		A.Id = "Jump.a";
		A.Primary = Button(GamepadButton::A);
		Jump.Bindings.push_back(A);
		Map.Actions = { Move, Jump };
		PF_CHECK(Tests, Input.SetActionMap(std::move(Map)).has_value());

		PF_CHECK(Tests, Adapter.Update(0, true, true, nullptr).Count == 0); // Failed initial GLFW sample.
		GamepadStateSample Neutral = NeutralPad();
		auto Events = Adapter.Update(0, true, true, &Neutral);
		PF_CHECK(Tests, Events.Count == GamepadStateSample::AxisCount + 1);
		PF_CHECK(Tests, Events.Values[0].Kind == InputEventKind::DeviceConnected);
		const uint64_t FirstIncarnation = Events.Values[0].DeviceInstance;
		PF_CHECK(Tests, Queue(Input, Events));
		Input.PublishFrame(Routing());

		GamepadStateSample Horizontal = Neutral;
		Horizontal.Axes[0] = 0.8f;
		Events = Adapter.Update(0, true, true, &Horizontal);
		PF_CHECK(Tests, Events.Count == 1 && Events.Values[0].Control.Code == static_cast<uint16_t>(GamepadAxis::LeftX));
		PF_CHECK(Tests, Queue(Input, Events));
		Input.PublishFrame(Routing());
		PF_CHECK(Tests, Input.FindAction("Move") && Input.FindAction("Move")->Held &&
			std::abs(Input.FindAction("Move")->X - 0.7647059f) < 0.001f && Input.FindAction("Move")->Y == 0.0f);

		GamepadStateSample Vertical = Horizontal;
		Vertical.Axes[0] = 0.0f;
		Vertical.Axes[1] = 0.8f;
		Events = Adapter.Update(0, true, true, &Vertical);
		PF_CHECK(Tests, Queue(Input, Events));
		Input.PublishFrame(Routing());
		PF_CHECK(Tests, Input.FindAction("Move") && Input.FindAction("Move")->X == 0.0f &&
			std::abs(Input.FindAction("Move")->Y - 0.7647059f) < 0.001f);
		GamepadStateSample Diagonal = Vertical;
		Diagonal.Axes[0] = 0.6f;
		PF_CHECK(Tests, Queue(Input, Adapter.Update(0, true, true, &Diagonal)));
		Input.PublishFrame(Routing());
		PF_CHECK(Tests, Input.FindAction("Move") && std::abs(Input.FindAction("Move")->X - 0.6f) < 0.001f &&
			std::abs(Input.FindAction("Move")->Y - 0.8f) < 0.001f);
		PF_CHECK(Tests, Queue(Input, Adapter.Update(0, true, true, &Neutral)));
		Input.PublishFrame(Routing());
		PF_CHECK(Tests, Input.FindAction("Move") && !Input.FindAction("Move")->Held);

		Events = Adapter.Update(0, false, false, nullptr);
		PF_CHECK(Tests, Events.Count == 1 && Events.Values[0].Kind == InputEventKind::DeviceDisconnected);
		PF_CHECK(Tests, Queue(Input, Events));
		Input.PublishFrame(Routing());
		PF_CHECK(Tests, Adapter.Update(0, true, false, nullptr).Count == 0); // Same slot is present, but not mapped.
		GamepadStateSample Held = Neutral;
		Held.Buttons[static_cast<size_t>(GamepadButton::A)] = true;
		Events = Adapter.Update(0, true, true, &Held); // Mapping becomes available on the reused slot.
		PF_CHECK(Tests, Events.Count == GamepadStateSample::AxisCount + 2);
		PF_CHECK(Tests, Events.Values[0].DeviceInstance != FirstIncarnation);
		PF_CHECK(Tests, Queue(Input, Events));
		Input.PublishFrame(Routing());
		PF_CHECK(Tests, Input.FindAction("Jump") && !Input.FindAction("Jump")->Held && !Input.FindAction("Jump")->Pressed);
		GamepadStateSample Released = Held;
		Released.Buttons[static_cast<size_t>(GamepadButton::A)] = false;
		PF_CHECK(Tests, Queue(Input, Adapter.Update(0, true, true, &Released)));
		Input.PublishFrame(Routing());
		PF_CHECK(Tests, Queue(Input, Adapter.Update(0, true, true, &Held)));
		Input.PublishFrame(Routing());
		PF_CHECK(Tests, Input.FindAction("Jump") && Input.FindAction("Jump")->Held && Input.FindAction("Jump")->Pressed);

		GamepadStateAdapter DelayedAdapter;
		GameplayInputService DelayedInput;
		GameplayActionMap DelayedMap;
		GameplayActionDefinition DelayedJump{ .Id = "Jump", .Type = ActionValueType::Digital };
		DelayedJump.Bindings.push_back(A);
		DelayedMap.Actions.push_back(DelayedJump);
		PF_CHECK(Tests, DelayedInput.SetActionMap(std::move(DelayedMap)).has_value());
		PF_CHECK(Tests, DelayedAdapter.Update(2, true, true, nullptr).Count == 0);
		GamepadStateSample HeldAfterDelay = Neutral;
		HeldAfterDelay.Buttons[static_cast<size_t>(GamepadButton::A)] = true;
		Events = DelayedAdapter.Update(2, true, true, &HeldAfterDelay);
		PF_CHECK(Tests, Events.Count == GamepadStateSample::AxisCount + 2);
		PF_CHECK(Tests, Queue(DelayedInput, Events));
		DelayedInput.PublishFrame(Routing());
		PF_CHECK(Tests, DelayedInput.FindAction("Jump") && !DelayedInput.FindAction("Jump")->Held &&
			!DelayedInput.FindAction("Jump")->Pressed);
		HeldAfterDelay.Buttons[static_cast<size_t>(GamepadButton::A)] = false;
		PF_CHECK(Tests, Queue(DelayedInput, DelayedAdapter.Update(2, true, true, &HeldAfterDelay)));
		DelayedInput.PublishFrame(Routing());
		HeldAfterDelay.Buttons[static_cast<size_t>(GamepadButton::A)] = true;
		PF_CHECK(Tests, Queue(DelayedInput, DelayedAdapter.Update(2, true, true, &HeldAfterDelay)));
		DelayedInput.PublishFrame(Routing());
		PF_CHECK(Tests, DelayedInput.FindAction("Jump") && DelayedInput.FindAction("Jump")->Held &&
			DelayedInput.FindAction("Jump")->Pressed);
	}

	void TestAnalogRearmUsesBindingDeadZones(TestRunner& Tests)
	{
		GameplayInputService Input;
		GameplayActionMap Map;
		GameplayActionDefinition StickAction{ .Id = "Stick", .Type = ActionValueType::Axis2D };
		ActionBinding Stick;
		Stick.Id = "Stick.primary";
		Stick.Kind = ActionBindingKind::Stick;
		Stick.Primary = Axis(GamepadAxis::LeftX);
		Stick.Secondary = Axis(GamepadAxis::LeftY);
		Stick.ScaleX = 1.0f;
		Stick.ScaleY = 1.0f;
		Stick.DeadZone = 0.15f;
		StickAction.Bindings.push_back(Stick);
		Map.Actions.push_back(StickAction);
		PF_CHECK(Tests, Input.SetActionMap(Map).has_value());
		const auto Device = [&Input](uint64_t Id)
		{ return Input.QueueEvent({ .Kind = InputEventKind::DeviceConnected, .DeviceInstance = Id }); };
		const auto Value = [&Input](GamepadAxis Code, uint64_t Id, float X)
		{ return Input.QueueEvent({ .Kind = InputEventKind::Value, .Control = Axis(Code), .DeviceInstance = Id, .X = X }); };
		PF_CHECK(Tests, Device(9));
		PF_CHECK(Tests, Value(GamepadAxis::LeftX, 9, 0.03f));
		PF_CHECK(Tests, Value(GamepadAxis::LeftY, 9, 0.02f));
		Input.PublishFrame(Routing());
		PF_CHECK(Tests, Input.FindAction("Stick") && !Input.FindAction("Stick")->Held);
		PF_CHECK(Tests, Value(GamepadAxis::LeftX, 9, 0.8f));
		PF_CHECK(Tests, Value(GamepadAxis::LeftY, 9, 0.7f));
		Input.PublishFrame(Routing());
		PF_CHECK(Tests, Input.FindAction("Stick") && Input.FindAction("Stick")->Held);

		GameplayInputService Shared;
		GameplayActionMap SharedMap;
		GameplayActionDefinition Narrow{ .Id = "Narrow", .Type = ActionValueType::Axis1D };
		Narrow.Bindings.push_back(AnalogBinding("Narrow.x", Axis(GamepadAxis::RightX), 0.10f));
		GameplayActionDefinition Wide{ .Id = "Wide", .Type = ActionValueType::Axis1D };
		Wide.Bindings.push_back(AnalogBinding("Wide.x", Axis(GamepadAxis::RightX), 0.30f));
		GameplayActionDefinition Trigger{ .Id = "Trigger", .Type = ActionValueType::Axis1D };
		Trigger.Bindings.push_back(AnalogBinding("Trigger.axis", Axis(GamepadAxis::LeftTrigger), 0.20f));
		SharedMap.Actions = { Narrow, Wide, Trigger };
		PF_CHECK(Tests, Shared.SetActionMap(std::move(SharedMap)).has_value());
		PF_CHECK(Tests, Shared.QueueEvent({ .Kind = InputEventKind::DeviceConnected, .DeviceInstance = 11 }));
		PF_CHECK(Tests, Shared.QueueEvent({ .Kind = InputEventKind::Value, .Control = Axis(GamepadAxis::RightX), .DeviceInstance = 11, .X = 0.12f }));
		PF_CHECK(Tests, Shared.QueueEvent({ .Kind = InputEventKind::Value, .Control = Axis(GamepadAxis::LeftTrigger), .DeviceInstance = 11, .X = 0.8f }));
		Shared.PublishFrame(Routing());
		PF_CHECK(Tests, Shared.QueueEvent({ .Kind = InputEventKind::Value, .Control = Axis(GamepadAxis::RightX), .DeviceInstance = 11, .X = 0.04f }));
		PF_CHECK(Tests, Shared.QueueEvent({ .Kind = InputEventKind::Value, .Control = Axis(GamepadAxis::LeftTrigger), .DeviceInstance = 11, .X = 0.0f }));
		Shared.PublishFrame(Routing());
		PF_CHECK(Tests, Shared.QueueEvent({ .Kind = InputEventKind::Value, .Control = Axis(GamepadAxis::RightX), .DeviceInstance = 11, .X = 0.20f }));
		PF_CHECK(Tests, Shared.QueueEvent({ .Kind = InputEventKind::Value, .Control = Axis(GamepadAxis::LeftTrigger), .DeviceInstance = 11, .X = 0.8f }));
		Shared.PublishFrame(Routing());
		PF_CHECK(Tests, Shared.FindAction("Narrow") && Shared.FindAction("Narrow")->Held);
		PF_CHECK(Tests, Shared.FindAction("Wide") && !Shared.FindAction("Wide")->Held);
		PF_CHECK(Tests, Shared.FindAction("Trigger") && Shared.FindAction("Trigger")->Held);

		GameplayInputService Radial;
		GameplayActionMap RadialMap;
		GameplayActionDefinition RadialAction{ .Id = "Radial", .Type = ActionValueType::Axis2D };
		Stick.Id = "Radial.stick";
		Stick.DeadZone = 0.15f;
		RadialAction.Bindings.push_back(Stick);
		RadialMap.Actions.push_back(RadialAction);
		PF_CHECK(Tests, Radial.SetActionMap(std::move(RadialMap)).has_value());
		PF_CHECK(Tests, Radial.QueueEvent({ .Kind = InputEventKind::DeviceConnected, .DeviceInstance = 13 }));
		PF_CHECK(Tests, Radial.QueueEvent({ .Kind = InputEventKind::Value,
			.Control = Axis(GamepadAxis::LeftX), .DeviceInstance = 13, .X = 0.06f }));
		PF_CHECK(Tests, Radial.QueueEvent({ .Kind = InputEventKind::Value,
			.Control = Axis(GamepadAxis::LeftY), .DeviceInstance = 13, .X = 0.06f }));
		Radial.PublishFrame(Routing()); // The radial magnitude is outside the inner-half rearm band.
		PF_CHECK(Tests, Radial.QueueEvent({ .Kind = InputEventKind::Value,
			.Control = Axis(GamepadAxis::LeftX), .DeviceInstance = 13, .X = 0.05f }));
		PF_CHECK(Tests, Radial.QueueEvent({ .Kind = InputEventKind::Value,
			.Control = Axis(GamepadAxis::LeftY), .DeviceInstance = 13, .X = 0.05f }));
		Radial.PublishFrame(Routing()); // The pair is now within the rearm band.
		PF_CHECK(Tests, Radial.QueueEvent({ .Kind = InputEventKind::Value,
			.Control = Axis(GamepadAxis::LeftX), .DeviceInstance = 13, .X = 0.20f }));
		PF_CHECK(Tests, Radial.QueueEvent({ .Kind = InputEventKind::Value,
			.Control = Axis(GamepadAxis::LeftY), .DeviceInstance = 13, .X = 0.20f }));
		Radial.PublishFrame(Routing());
		PF_CHECK(Tests, Radial.FindAction("Radial") && Radial.FindAction("Radial")->Held);

		InputRoutingState NoGameplay;
		Shared.PublishFrame(NoGameplay); // Owner change quarantines held analog controls.
		PF_CHECK(Tests, Shared.QueueEvent({ .Kind = InputEventKind::Value,
			.Control = Axis(GamepadAxis::RightX), .DeviceInstance = 11, .X = 0.06f })); // Between neutral/rearm and activation thresholds.
		PF_CHECK(Tests, Shared.QueueEvent({ .Kind = InputEventKind::Value,
			.Control = Axis(GamepadAxis::LeftTrigger), .DeviceInstance = 11, .X = 0.0f }));
		Shared.PublishFrame(NoGameplay);
		PF_CHECK(Tests, Shared.QueueEvent({ .Kind = InputEventKind::Value,
			.Control = Axis(GamepadAxis::RightX), .DeviceInstance = 11, .X = 0.051f })); // Small jitter stays inside the hysteresis band.
		Shared.PublishFrame(NoGameplay);
		PF_CHECK(Tests, Shared.QueueEvent({ .Kind = InputEventKind::Value,
			.Control = Axis(GamepadAxis::RightX), .DeviceInstance = 11, .X = 0.05f })); // Inclusive inner-half boundary rearms.
		Shared.PublishFrame(NoGameplay);
		Shared.PublishFrame(Routing());
		PF_CHECK(Tests, Shared.FindAction("Narrow") && !Shared.FindAction("Narrow")->Held);
		PF_CHECK(Tests, Shared.QueueEvent({ .Kind = InputEventKind::Value,
			.Control = Axis(GamepadAxis::RightX), .DeviceInstance = 11, .X = 0.20f }));
		PF_CHECK(Tests, Shared.QueueEvent({ .Kind = InputEventKind::Value,
			.Control = Axis(GamepadAxis::LeftTrigger), .DeviceInstance = 11, .X = 0.8f }));
		Shared.PublishFrame(Routing());
		PF_CHECK(Tests, Shared.FindAction("Narrow") && Shared.FindAction("Narrow")->Held);
		PF_CHECK(Tests, Shared.FindAction("Trigger") && Shared.FindAction("Trigger")->Held);
	}

	void TestTransientMouseControlsAcrossMapChanges(TestRunner& Tests)
	{
		GameplayInputService Input;
		GameplayActionMap Map;
		GameplayActionDefinition MotionAction{ .Id = "Motion", .Type = ActionValueType::Axis1D };
		MotionAction.Bindings.push_back(AnalogBinding("Motion.x", Motion(MouseControl::MotionX)));
		GameplayActionDefinition WheelAction{ .Id = "Wheel", .Type = ActionValueType::Axis1D };
		WheelAction.Bindings.push_back(AnalogBinding("Wheel.y", Wheel(MouseControl::WheelY)));
		GameplayActionDefinition DigitalAction{ .Id = "Digital", .Type = ActionValueType::Digital };
		ActionBinding Key;
		Key.Id = "Digital.key";
		Key.Primary = { InputDevice::Keyboard, static_cast<uint16_t>(InputKey::D) };
		DigitalAction.Bindings.push_back(Key);
		Map.Actions = { MotionAction, WheelAction, DigitalAction };
		PF_CHECK(Tests, Input.SetActionMap(Map).has_value());
		const auto Pointer = [&Input](float X)
		{ return Input.QueueEvent({ .Kind = InputEventKind::PointerMoved, .Control = Motion(MouseControl::MotionX),
			.Window = TestWindow, .X = X, .Y = 5.0f }); };
		const auto Scroll = [&Input]()
		{ return Input.QueueEvent({ .Kind = InputEventKind::Scrolled, .Control = Wheel(MouseControl::WheelY),
			.Window = TestWindow, .Y = 1.0f }); };
		PF_CHECK(Tests, Pointer(10.0f));
		Input.PublishFrame(Routing());
		PF_CHECK(Tests, Pointer(11.0f));
		PF_CHECK(Tests, Scroll());
		Input.PublishFrame(Routing());
		PF_CHECK(Tests, Input.FindAction("Motion") && Input.FindAction("Motion")->X == 1.0f);
		PF_CHECK(Tests, Input.FindAction("Wheel") && Input.FindAction("Wheel")->X == 1.0f);
		PF_CHECK(Tests, Input.QueueEvent({ .Kind = InputEventKind::Pressed, .Control = Key.Primary, .Window = TestWindow }));
		Input.PublishFrame(Routing());
		PF_CHECK(Tests, Input.FindAction("Digital") && Input.FindAction("Digital")->Held);
		GameplayActionDefinition Other{ .Id = "Other", .Type = ActionValueType::Axis1D };
		Other.Bindings.push_back(AnalogBinding("Other.key", { InputDevice::Keyboard, static_cast<uint16_t>(InputKey::D) }));
		Map.Actions.push_back(Other);
		PF_CHECK(Tests, Input.SetActionMap(Map).has_value()); // Unrelated rebind must not poison transient controls.
		Input.PublishFrame(Routing());
		const auto ClearedMapEdges = Input.BeginSimulationStep();
		for (const auto& Entry : ClearedMapEdges.Actions)
			PF_CHECK(Tests, !Entry.Value.Pressed);
		PF_CHECK(Tests, Input.FindAction("Digital") && !Input.FindAction("Digital")->Held);
		PF_CHECK(Tests, Pointer(13.0f)); // A map change clears the pointer baseline; this event creates a new one.
		PF_CHECK(Tests, Scroll());
		Input.PublishFrame(Routing());
		PF_CHECK(Tests, Input.FindAction("Motion") && !Input.FindAction("Motion")->Held);
		PF_CHECK(Tests, Input.FindAction("Wheel") && Input.FindAction("Wheel")->X == 1.0f);
		PF_CHECK(Tests, Input.QueueEvent({ .Kind = InputEventKind::Released, .Control = Key.Primary, .Window = TestWindow }));
		Input.PublishFrame(Routing());
		PF_CHECK(Tests, Pointer(14.0f));
		Input.PublishFrame(Routing());
		PF_CHECK(Tests, Input.FindAction("Motion") && Input.FindAction("Motion")->X == 1.0f);
		PF_CHECK(Tests, Input.QueueEvent({ .Kind = InputEventKind::Pressed, .Control = Key.Primary, .Window = TestWindow }));
		Input.PublishFrame(Routing());
		PF_CHECK(Tests, Input.FindAction("Digital") && Input.FindAction("Digital")->Held);
		PF_CHECK(Tests, Input.QueueEvent({ .Kind = InputEventKind::Released, .Control = Key.Primary, .Window = TestWindow }));
		Input.PublishFrame(Routing());

		PF_CHECK(Tests, Scroll());
		PF_CHECK(Tests, Input.QueueEvent({ .Kind = InputEventKind::FocusLost, .Window = TestWindow }));
		Input.PublishFrame(Routing());
		PF_CHECK(Tests, Input.FindAction("Wheel") && !Input.FindAction("Wheel")->Held);
		PF_CHECK(Tests, Pointer(40.0f));
		Input.PublishFrame(Routing());
		PF_CHECK(Tests, Input.FindAction("Wheel") && !Input.FindAction("Wheel")->Held);
		PF_CHECK(Tests, Input.FindAction("Motion") && !Input.FindAction("Motion")->Held);
		PF_CHECK(Tests, Pointer(41.0f));
		PF_CHECK(Tests, Input.QueueEvent({ .Kind = InputEventKind::CaptureCancelled, .Window = TestWindow }));
		Input.PublishFrame(Routing());
		PF_CHECK(Tests, Pointer(70.0f));
		Input.PublishFrame(Routing());
		PF_CHECK(Tests, Pointer(71.0f));
		PF_CHECK(Tests, Scroll());
		PF_CHECK(Tests, Input.QueueEvent({ .Kind = InputEventKind::CaptureCancelled, .Window = TestWindow }));
		Input.PublishFrame(Routing());
		PF_CHECK(Tests, Input.FindAction("Motion") && !Input.FindAction("Motion")->Held);
		PF_CHECK(Tests, Input.FindAction("Wheel") && !Input.FindAction("Wheel")->Held);
		PF_CHECK(Tests, Pointer(72.0f));
		Input.PublishFrame(Routing());
		PF_CHECK(Tests, Input.FindAction("Motion") && !Input.FindAction("Motion")->Held);
		PF_CHECK(Tests, Pointer(73.0f));
		Input.PublishFrame(Routing());
		PF_CHECK(Tests, Input.FindAction("Motion") && Input.FindAction("Motion")->X == 1.0f);

		PF_CHECK(Tests, Input.SetActionMap(Map).has_value());
		Input.PublishFrame(Routing());
		PF_CHECK(Tests, Input.FindAction("Motion") && !Input.FindAction("Motion")->Held);
		PF_CHECK(Tests, Input.FindAction("Wheel") && !Input.FindAction("Wheel")->Held);
		PF_CHECK(Tests, Pointer(12.0f));
		PF_CHECK(Tests, Scroll());
		Input.PublishFrame(Routing());
		PF_CHECK(Tests, Input.FindAction("Motion") && !Input.FindAction("Motion")->Held);
		PF_CHECK(Tests, Input.FindAction("Wheel") && Input.FindAction("Wheel")->X == 1.0f);
		PF_CHECK(Tests, Pointer(13.0f));
		Input.PublishFrame(Routing());
		PF_CHECK(Tests, Input.FindAction("Motion") && Input.FindAction("Motion")->X == 1.0f);
		PF_CHECK(Tests, Input.FindAction("Wheel") && !Input.FindAction("Wheel")->Held);

		Input.Reset();
		Input.PublishFrame(Routing());
		for (size_t Index = 0; Index < GameplayInputService::MaximumQueuedEvents; ++Index)
			PF_CHECK(Tests, Input.QueueEvent({ .Kind = InputEventKind::Scrolled, .Control = Wheel(MouseControl::WheelY),
				.Window = TestWindow, .Y = 1.0f }));
		PF_CHECK(Tests, !Input.QueueEvent({ .Kind = InputEventKind::Scrolled, .Control = Wheel(MouseControl::WheelY),
			.Window = TestWindow, .Y = 1.0f }));
		Input.PublishFrame(Routing());
		PF_CHECK(Tests, Input.FindAction("Wheel") && !Input.FindAction("Wheel")->Held);
		PF_CHECK(Tests, Pointer(90.0f));
		Input.PublishFrame(Routing());
		PF_CHECK(Tests, Pointer(91.0f));
		PF_CHECK(Tests, Scroll());
		Input.PublishFrame(Routing());
		PF_CHECK(Tests, Input.FindAction("Motion") && Input.FindAction("Motion")->X == 1.0f);
		PF_CHECK(Tests, Input.FindAction("Wheel") && Input.FindAction("Wheel")->X == 1.0f);

		Input.Reset();
		Input.PublishFrame(Routing());
		PF_CHECK(Tests, Pointer(20.0f));
		Input.PublishFrame(Routing());
		PF_CHECK(Tests, Input.FindAction("Motion") && !Input.FindAction("Motion")->Held);
		PF_CHECK(Tests, Pointer(21.0f));
		PF_CHECK(Tests, Scroll());
		Input.PublishFrame(Routing());
		PF_CHECK(Tests, Input.FindAction("Motion") && Input.FindAction("Motion")->X == 1.0f);
		PF_CHECK(Tests, Input.FindAction("Wheel") && Input.FindAction("Wheel")->X == 1.0f);
	}
}

void RunGameplayInputLifecycleTests(TestRunner& Tests)
{
	TestGamepadSampleNormalization(Tests);
	TestInitialGamepadSampleAndReconnect(Tests);
	TestAnalogRearmUsesBindingDeadZones(Tests);
	TestTransientMouseControlsAcrossMapChanges(Tests);
}
