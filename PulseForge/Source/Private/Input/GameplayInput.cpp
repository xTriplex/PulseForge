#include "Input/GameplayInput.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace PulseForge
{
	namespace
	{
		using Json = nlohmann::json;

		constexpr size_t MaximumJsonBytes = 1024 * 1024;
		constexpr size_t MaximumActionIdBytes = 64;
		constexpr size_t MaximumBindingsPerAction = 64;
		constexpr size_t MaximumBindingsTotal = 4096;
		constexpr float MaximumBindingScale = 100.0f;
		constexpr float ActionActivationEpsilon = 0.0001f;

		struct NamedControl
		{
			InputControl Control;
			std::string_view Name;
		};

		constexpr auto KeyboardControl(InputKey Key)
		{
			return InputControl{ InputDevice::Keyboard, static_cast<uint16_t>(Key) };
		}

		constexpr auto MouseButtonControl(MouseControl Button)
		{
			return InputControl{ InputDevice::MouseButton, static_cast<uint16_t>(Button) };
		}

		constexpr auto MouseMotionControl(MouseControl Axis)
		{
			return InputControl{ InputDevice::MouseMotion, static_cast<uint16_t>(Axis) };
		}

		constexpr auto MouseWheelControl(MouseControl Axis)
		{
			return InputControl{ InputDevice::MouseWheel, static_cast<uint16_t>(Axis) };
		}

		constexpr auto GamepadButtonControl(GamepadButton Button)
		{
			return InputControl{ InputDevice::GamepadButton, static_cast<uint16_t>(Button) };
		}

		constexpr auto GamepadAxisControl(GamepadAxis Axis)
		{
			return InputControl{ InputDevice::GamepadAxis, static_cast<uint16_t>(Axis) };
		}

		constexpr NamedControl NamedControls[] = {
			{ KeyboardControl(InputKey::Space), "Key.Space" },
			{ KeyboardControl(InputKey::Apostrophe), "Key.Apostrophe" },
			{ KeyboardControl(InputKey::Comma), "Key.Comma" },
			{ KeyboardControl(InputKey::Minus), "Key.Minus" },
			{ KeyboardControl(InputKey::Period), "Key.Period" },
			{ KeyboardControl(InputKey::Slash), "Key.Slash" },
			{ KeyboardControl(InputKey::Digit0), "Key.Digit0" },
			{ KeyboardControl(InputKey::Digit1), "Key.Digit1" },
			{ KeyboardControl(InputKey::Digit2), "Key.Digit2" },
			{ KeyboardControl(InputKey::Digit3), "Key.Digit3" },
			{ KeyboardControl(InputKey::Digit4), "Key.Digit4" },
			{ KeyboardControl(InputKey::Digit5), "Key.Digit5" },
			{ KeyboardControl(InputKey::Digit6), "Key.Digit6" },
			{ KeyboardControl(InputKey::Digit7), "Key.Digit7" },
			{ KeyboardControl(InputKey::Digit8), "Key.Digit8" },
			{ KeyboardControl(InputKey::Digit9), "Key.Digit9" },
			{ KeyboardControl(InputKey::Semicolon), "Key.Semicolon" },
			{ KeyboardControl(InputKey::Equal), "Key.Equal" },
			{ KeyboardControl(InputKey::A), "Key.A" }, { KeyboardControl(InputKey::B), "Key.B" },
			{ KeyboardControl(InputKey::C), "Key.C" }, { KeyboardControl(InputKey::D), "Key.D" },
			{ KeyboardControl(InputKey::E), "Key.E" }, { KeyboardControl(InputKey::F), "Key.F" },
			{ KeyboardControl(InputKey::G), "Key.G" }, { KeyboardControl(InputKey::H), "Key.H" },
			{ KeyboardControl(InputKey::I), "Key.I" }, { KeyboardControl(InputKey::J), "Key.J" },
			{ KeyboardControl(InputKey::K), "Key.K" }, { KeyboardControl(InputKey::L), "Key.L" },
			{ KeyboardControl(InputKey::M), "Key.M" }, { KeyboardControl(InputKey::N), "Key.N" },
			{ KeyboardControl(InputKey::O), "Key.O" }, { KeyboardControl(InputKey::P), "Key.P" },
			{ KeyboardControl(InputKey::Q), "Key.Q" }, { KeyboardControl(InputKey::R), "Key.R" },
			{ KeyboardControl(InputKey::S), "Key.S" }, { KeyboardControl(InputKey::T), "Key.T" },
			{ KeyboardControl(InputKey::U), "Key.U" }, { KeyboardControl(InputKey::V), "Key.V" },
			{ KeyboardControl(InputKey::W), "Key.W" }, { KeyboardControl(InputKey::X), "Key.X" },
			{ KeyboardControl(InputKey::Y), "Key.Y" }, { KeyboardControl(InputKey::Z), "Key.Z" },
			{ KeyboardControl(InputKey::LeftBracket), "Key.LeftBracket" },
			{ KeyboardControl(InputKey::Backslash), "Key.Backslash" },
			{ KeyboardControl(InputKey::RightBracket), "Key.RightBracket" },
			{ KeyboardControl(InputKey::GraveAccent), "Key.GraveAccent" },
			{ KeyboardControl(InputKey::Escape), "Key.Escape" },
			{ KeyboardControl(InputKey::Enter), "Key.Enter" },
			{ KeyboardControl(InputKey::Tab), "Key.Tab" },
			{ KeyboardControl(InputKey::Backspace), "Key.Backspace" },
			{ KeyboardControl(InputKey::Insert), "Key.Insert" },
			{ KeyboardControl(InputKey::Delete), "Key.Delete" },
			{ KeyboardControl(InputKey::Right), "Key.Right" },
			{ KeyboardControl(InputKey::Left), "Key.Left" },
			{ KeyboardControl(InputKey::Down), "Key.Down" },
			{ KeyboardControl(InputKey::Up), "Key.Up" },
			{ KeyboardControl(InputKey::PageUp), "Key.PageUp" },
			{ KeyboardControl(InputKey::PageDown), "Key.PageDown" },
			{ KeyboardControl(InputKey::Home), "Key.Home" },
			{ KeyboardControl(InputKey::End), "Key.End" },
			{ KeyboardControl(InputKey::CapsLock), "Key.CapsLock" },
			{ KeyboardControl(InputKey::ScrollLock), "Key.ScrollLock" },
			{ KeyboardControl(InputKey::NumLock), "Key.NumLock" },
			{ KeyboardControl(InputKey::PrintScreen), "Key.PrintScreen" },
			{ KeyboardControl(InputKey::Pause), "Key.Pause" },
			{ KeyboardControl(InputKey::F1), "Key.F1" }, { KeyboardControl(InputKey::F2), "Key.F2" },
			{ KeyboardControl(InputKey::F3), "Key.F3" }, { KeyboardControl(InputKey::F4), "Key.F4" },
			{ KeyboardControl(InputKey::F5), "Key.F5" }, { KeyboardControl(InputKey::F6), "Key.F6" },
			{ KeyboardControl(InputKey::F7), "Key.F7" }, { KeyboardControl(InputKey::F8), "Key.F8" },
			{ KeyboardControl(InputKey::F9), "Key.F9" }, { KeyboardControl(InputKey::F10), "Key.F10" },
			{ KeyboardControl(InputKey::F11), "Key.F11" }, { KeyboardControl(InputKey::F12), "Key.F12" },
			{ KeyboardControl(InputKey::F13), "Key.F13" }, { KeyboardControl(InputKey::F14), "Key.F14" },
			{ KeyboardControl(InputKey::F15), "Key.F15" }, { KeyboardControl(InputKey::F16), "Key.F16" },
			{ KeyboardControl(InputKey::F17), "Key.F17" }, { KeyboardControl(InputKey::F18), "Key.F18" },
			{ KeyboardControl(InputKey::F19), "Key.F19" }, { KeyboardControl(InputKey::F20), "Key.F20" },
			{ KeyboardControl(InputKey::F21), "Key.F21" }, { KeyboardControl(InputKey::F22), "Key.F22" },
			{ KeyboardControl(InputKey::F23), "Key.F23" }, { KeyboardControl(InputKey::F24), "Key.F24" },
			{ KeyboardControl(InputKey::F25), "Key.F25" },
			{ KeyboardControl(InputKey::Keypad0), "Key.Keypad0" },
			{ KeyboardControl(InputKey::Keypad1), "Key.Keypad1" },
			{ KeyboardControl(InputKey::Keypad2), "Key.Keypad2" },
			{ KeyboardControl(InputKey::Keypad3), "Key.Keypad3" },
			{ KeyboardControl(InputKey::Keypad4), "Key.Keypad4" },
			{ KeyboardControl(InputKey::Keypad5), "Key.Keypad5" },
			{ KeyboardControl(InputKey::Keypad6), "Key.Keypad6" },
			{ KeyboardControl(InputKey::Keypad7), "Key.Keypad7" },
			{ KeyboardControl(InputKey::Keypad8), "Key.Keypad8" },
			{ KeyboardControl(InputKey::Keypad9), "Key.Keypad9" },
			{ KeyboardControl(InputKey::KeypadDecimal), "Key.KeypadDecimal" },
			{ KeyboardControl(InputKey::KeypadDivide), "Key.KeypadDivide" },
			{ KeyboardControl(InputKey::KeypadMultiply), "Key.KeypadMultiply" },
			{ KeyboardControl(InputKey::KeypadSubtract), "Key.KeypadSubtract" },
			{ KeyboardControl(InputKey::KeypadAdd), "Key.KeypadAdd" },
			{ KeyboardControl(InputKey::KeypadEnter), "Key.KeypadEnter" },
			{ KeyboardControl(InputKey::KeypadEqual), "Key.KeypadEqual" },
			{ KeyboardControl(InputKey::LeftShift), "Key.LeftShift" },
			{ KeyboardControl(InputKey::LeftControl), "Key.LeftControl" },
			{ KeyboardControl(InputKey::LeftAlt), "Key.LeftAlt" },
			{ KeyboardControl(InputKey::LeftSuper), "Key.LeftSuper" },
			{ KeyboardControl(InputKey::RightShift), "Key.RightShift" },
			{ KeyboardControl(InputKey::RightControl), "Key.RightControl" },
			{ KeyboardControl(InputKey::RightAlt), "Key.RightAlt" },
			{ KeyboardControl(InputKey::RightSuper), "Key.RightSuper" },
			{ KeyboardControl(InputKey::Menu), "Key.Menu" },
			{ MouseButtonControl(MouseControl::Left), "Mouse.Left" },
			{ MouseButtonControl(MouseControl::Right), "Mouse.Right" },
			{ MouseButtonControl(MouseControl::Middle), "Mouse.Middle" },
			{ MouseButtonControl(MouseControl::Button4), "Mouse.Button4" },
			{ MouseButtonControl(MouseControl::Button5), "Mouse.Button5" },
			{ MouseButtonControl(MouseControl::Button6), "Mouse.Button6" },
			{ MouseButtonControl(MouseControl::Button7), "Mouse.Button7" },
			{ MouseButtonControl(MouseControl::Button8), "Mouse.Button8" },
			{ MouseMotionControl(MouseControl::MotionX), "Mouse.MotionX" },
			{ MouseMotionControl(MouseControl::MotionY), "Mouse.MotionY" },
			{ MouseWheelControl(MouseControl::WheelX), "Mouse.WheelX" },
			{ MouseWheelControl(MouseControl::WheelY), "Mouse.WheelY" },
			{ GamepadButtonControl(GamepadButton::A), "GamepadButton.A" },
			{ GamepadButtonControl(GamepadButton::B), "GamepadButton.B" },
			{ GamepadButtonControl(GamepadButton::X), "GamepadButton.X" },
			{ GamepadButtonControl(GamepadButton::Y), "GamepadButton.Y" },
			{ GamepadButtonControl(GamepadButton::LeftBumper), "GamepadButton.LeftBumper" },
			{ GamepadButtonControl(GamepadButton::RightBumper), "GamepadButton.RightBumper" },
			{ GamepadButtonControl(GamepadButton::Back), "GamepadButton.Back" },
			{ GamepadButtonControl(GamepadButton::Start), "GamepadButton.Start" },
			{ GamepadButtonControl(GamepadButton::Guide), "GamepadButton.Guide" },
			{ GamepadButtonControl(GamepadButton::LeftThumb), "GamepadButton.LeftThumb" },
			{ GamepadButtonControl(GamepadButton::RightThumb), "GamepadButton.RightThumb" },
			{ GamepadButtonControl(GamepadButton::DpadUp), "GamepadButton.DpadUp" },
			{ GamepadButtonControl(GamepadButton::DpadRight), "GamepadButton.DpadRight" },
			{ GamepadButtonControl(GamepadButton::DpadDown), "GamepadButton.DpadDown" },
			{ GamepadButtonControl(GamepadButton::DpadLeft), "GamepadButton.DpadLeft" },
			{ GamepadAxisControl(GamepadAxis::LeftX), "GamepadAxis.LeftX" },
			{ GamepadAxisControl(GamepadAxis::LeftY), "GamepadAxis.LeftY" },
			{ GamepadAxisControl(GamepadAxis::RightX), "GamepadAxis.RightX" },
			{ GamepadAxisControl(GamepadAxis::RightY), "GamepadAxis.RightY" },
			{ GamepadAxisControl(GamepadAxis::LeftTrigger), "GamepadAxis.LeftTrigger" },
			{ GamepadAxisControl(GamepadAxis::RightTrigger), "GamepadAxis.RightTrigger" }
		};

		[[nodiscard]] bool IsValidDevice(InputDevice Device) noexcept
		{
			return Device <= InputDevice::GamepadAxis;
		}

		[[nodiscard]] bool IsValidControl(InputControl Control) noexcept
		{
			if (!IsValidDevice(Control.Device)) return false;
			return std::any_of(std::begin(NamedControls), std::end(NamedControls),
				[Control](const NamedControl& Named) { return Named.Control == Control; });
		}

		[[nodiscard]] bool IsDigitalControl(InputControl Control) noexcept
		{
			return Control.Device == InputDevice::Keyboard || Control.Device == InputDevice::MouseButton ||
				Control.Device == InputDevice::GamepadButton;
		}

		[[nodiscard]] bool IsAxisControl(InputControl Control) noexcept
		{
			return Control.Device == InputDevice::GamepadAxis || Control.Device == InputDevice::MouseMotion ||
				Control.Device == InputDevice::MouseWheel;
		}

		[[nodiscard]] bool IsContextValid(InputContext Context) noexcept
		{
			return Context <= InputContext::RuntimeUI;
		}

		[[nodiscard]] bool IsDeviceMaskValid(InputDeviceMask Mask) noexcept
		{
			return (static_cast<uint8_t>(Mask) & ~static_cast<uint8_t>(InputDeviceMask::All)) == 0;
		}

		[[nodiscard]] std::optional<InputDeviceMask> GetMask(InputDevice Device) noexcept
		{
			switch (Device)
			{
			case InputDevice::Keyboard: return InputDeviceMask::Keyboard;
			case InputDevice::MouseButton:
			case InputDevice::MouseMotion:
			case InputDevice::MouseWheel: return InputDeviceMask::Mouse;
			case InputDevice::GamepadButton:
			case InputDevice::GamepadAxis: return InputDeviceMask::Gamepad;
			}
			return std::nullopt;
		}

		[[nodiscard]] bool IsValidActionId(std::string_view Id) noexcept
		{
			if (Id.empty() || Id.size() > MaximumActionIdBytes) return false;
			const auto IsStart = [](unsigned char Character) { return (Character >= 'A' && Character <= 'Z') ||
				(Character >= 'a' && Character <= 'z') || Character == '_'; };
			const auto IsContinue = [IsStart](unsigned char Character) {
				return IsStart(Character) || (Character >= '0' && Character <= '9') || Character == '.' || Character == '-';
			};
			if (!IsStart(static_cast<unsigned char>(Id.front()))) return false;
			return std::all_of(Id.begin() + 1, Id.end(), [IsContinue](char Character) {
				return IsContinue(static_cast<unsigned char>(Character));
			});
		}

		[[nodiscard]] const char* ContextName(InputContext Context) noexcept
		{
			switch (Context)
			{
			case InputContext::Gameplay: return "Gameplay";
			case InputContext::EditorShortcuts: return "EditorShortcuts";
			case InputContext::ViewportNavigation: return "ViewportNavigation";
			case InputContext::TextEntry: return "TextEntry";
			case InputContext::RuntimeUI: return "RuntimeUI";
			}
			return "";
		}

		[[nodiscard]] std::optional<InputContext> ParseContext(std::string_view Name) noexcept
		{
			for (uint8_t Value = 0; Value <= static_cast<uint8_t>(InputContext::RuntimeUI); ++Value)
			{
				const auto Context = static_cast<InputContext>(Value);
				if (Name == ContextName(Context)) return Context;
			}
			return std::nullopt;
		}

		[[nodiscard]] const char* ActionTypeName(ActionValueType Type) noexcept
		{
			switch (Type)
			{
			case ActionValueType::Digital: return "Digital";
			case ActionValueType::Axis1D: return "Axis1D";
			case ActionValueType::Axis2D: return "Axis2D";
			}
			return "";
		}

		[[nodiscard]] std::optional<ActionValueType> ParseActionType(std::string_view Name) noexcept
		{
			for (uint8_t Value = 0; Value <= static_cast<uint8_t>(ActionValueType::Axis2D); ++Value)
			{
				const auto Type = static_cast<ActionValueType>(Value);
				if (Name == ActionTypeName(Type)) return Type;
			}
			return std::nullopt;
		}

		[[nodiscard]] const char* BindingKindName(ActionBindingKind Kind) noexcept
		{
			switch (Kind)
			{
			case ActionBindingKind::Button: return "Button";
			case ActionBindingKind::Axis: return "Axis";
			case ActionBindingKind::Stick: return "Stick";
			}
			return "";
		}

		[[nodiscard]] std::optional<ActionBindingKind> ParseBindingKind(std::string_view Name) noexcept
		{
			for (uint8_t Value = 0; Value <= static_cast<uint8_t>(ActionBindingKind::Stick); ++Value)
			{
				const auto Kind = static_cast<ActionBindingKind>(Value);
				if (Name == BindingKindName(Kind)) return Kind;
			}
			return std::nullopt;
		}

		[[nodiscard]] GameplayInputError Error(std::string Message)
		{
			return { std::move(Message) };
		}

		[[nodiscard]] std::string DescribeIndex(std::string_view Prefix, size_t Index)
		{
			return std::string(Prefix) + "[" + std::to_string(Index) + "]";
		}

		[[nodiscard]] float ClampFinite(float Value, float Minimum, float Maximum) noexcept
		{
			if (!std::isfinite(Value)) return 0.0f;
			return (std::clamp)(Value, Minimum, Maximum);
		}

		[[nodiscard]] bool IsActionContextActive(const InputRoutingState& Routing, InputContext Context) noexcept
		{
			const size_t Count = (std::min)(Routing.ContextCount, Routing.Contexts.size());
			for (size_t Index = 0; Index < Count; ++Index)
				if (Routing.Contexts[Index].Active && Routing.Contexts[Index].Context == Context) return true;
			return false;
		}

		[[nodiscard]] InputDeviceMask DeviceMask(InputDevice Device) noexcept
		{
			return GetMask(Device).value_or(InputDeviceMask::None);
		}
	} // namespace

	std::string_view GetInputControlName(InputControl Control) noexcept
	{
		for (const NamedControl& Named : NamedControls)
			if (Named.Control == Control) return Named.Name;
		return {};
	}

	std::optional<InputControl> ParseInputControlName(std::string_view Name) noexcept
	{
		for (const NamedControl& Named : NamedControls)
			if (Named.Name == Name) return Named.Control;
		return std::nullopt;
	}

	std::expected<void, GameplayInputError> ValidateGameplayActionMap(const GameplayActionMap& Map)
	{
		if (Map.Version != 1) return std::unexpected(Error("Gameplay action map version must be 1."));
		if (Map.Actions.size() > GameplayInputService::MaximumActions)
			return std::unexpected(Error("Gameplay action map exceeds the 256 action limit."));

		std::unordered_set<std::string_view> Ids;
		size_t TotalBindings = 0;
		for (size_t ActionIndex = 0; ActionIndex < Map.Actions.size(); ++ActionIndex)
		{
			const GameplayActionDefinition& Action = Map.Actions[ActionIndex];
			const std::string Location = DescribeIndex("actions", ActionIndex);
			if (!IsValidActionId(Action.Id))
				return std::unexpected(Error(Location + ".id must be a 1-64 character stable ASCII identifier."));
			if (!Ids.emplace(Action.Id).second)
				return std::unexpected(Error(Location + ".id duplicates action '" + Action.Id + "'."));
			if (Action.Type > ActionValueType::Axis2D)
				return std::unexpected(Error(Location + ".type is unsupported."));
			if (!IsContextValid(Action.Context))
				return std::unexpected(Error(Location + ".context is unsupported."));
			if (Action.Bindings.size() > MaximumBindingsPerAction)
				return std::unexpected(Error(Location + ".bindings exceeds the 64 binding limit."));
			std::unordered_set<std::string_view> BindingIds;
			TotalBindings += Action.Bindings.size();
			if (TotalBindings > MaximumBindingsTotal)
				return std::unexpected(Error("Gameplay action map exceeds the total binding limit."));

			for (size_t BindingIndex = 0; BindingIndex < Action.Bindings.size(); ++BindingIndex)
			{
				const ActionBinding& Binding = Action.Bindings[BindingIndex];
				const std::string BindingLocation = Location + ".bindings[" + std::to_string(BindingIndex) + "]";
				if (!IsValidActionId(Binding.Id))
					return std::unexpected(Error(BindingLocation + ".id must be a 1-64 character stable ASCII identifier."));
				if (!BindingIds.emplace(Binding.Id).second)
					return std::unexpected(Error(BindingLocation + ".id duplicates binding '" + Binding.Id + "' in action '" + Action.Id + "'."));
				if (Binding.Kind > ActionBindingKind::Stick)
					return std::unexpected(Error(BindingLocation + ".kind is unsupported."));
				if (!IsValidControl(Binding.Primary))
					return std::unexpected(Error(BindingLocation + ".primary is not a supported named control."));
				if (!std::isfinite(Binding.ScaleX) || !std::isfinite(Binding.ScaleY) ||
					std::abs(Binding.ScaleX) > MaximumBindingScale || std::abs(Binding.ScaleY) > MaximumBindingScale)
					return std::unexpected(Error(BindingLocation + ".scale must be finite and within [-100, 100]."));
				if (!std::isfinite(Binding.DeadZone) || Binding.DeadZone < 0.0f || Binding.DeadZone > 0.95f)
					return std::unexpected(Error(BindingLocation + ".deadZone must be finite and in [0, 0.95]."));
				if (Binding.ModifierCount > Binding.Modifiers.size())
					return std::unexpected(Error(BindingLocation + ".modifier count exceeds the four modifier limit."));
				for (size_t ModifierIndex = 0; ModifierIndex < Binding.ModifierCount; ++ModifierIndex)
				{
					const InputControl Modifier = Binding.Modifiers[ModifierIndex];
					if (!IsValidControl(Modifier) || !IsDigitalControl(Modifier))
						return std::unexpected(Error(BindingLocation + ".modifiers must be supported digital controls."));
					if (Modifier == Binding.Primary)
						return std::unexpected(Error(BindingLocation + " cannot use its primary control as a modifier."));
					for (size_t Earlier = 0; Earlier < ModifierIndex; ++Earlier)
						if (Binding.Modifiers[Earlier] == Modifier)
							return std::unexpected(Error(BindingLocation + " contains a duplicate modifier."));
				}

				if (Binding.Kind == ActionBindingKind::Button)
				{
					if (!IsDigitalControl(Binding.Primary))
						return std::unexpected(Error(BindingLocation + " Button bindings require a digital primary control."));
				}
				else if (Binding.Kind == ActionBindingKind::Axis)
				{
					if (Action.Type == ActionValueType::Digital)
						return std::unexpected(Error(BindingLocation + " Axis bindings require an Axis1D or Axis2D action."));
					if (!IsDigitalControl(Binding.Primary) && !IsAxisControl(Binding.Primary))
						return std::unexpected(Error(BindingLocation + ".primary is not an axis or digital control."));
				}
				else
				{
					if (Action.Type != ActionValueType::Axis2D || !IsAxisControl(Binding.Primary) ||
						!IsAxisControl(Binding.Secondary))
						return std::unexpected(Error(BindingLocation + " Stick bindings require an Axis2D action and two axis controls."));
					if (Binding.Primary.Device != Binding.Secondary.Device)
						return std::unexpected(Error(BindingLocation + " Stick controls must use the same device class."));
					if ((Binding.Primary.Device == InputDevice::GamepadAxis && Binding.Primary.Code == Binding.Secondary.Code) ||
						(Binding.Primary.Device != InputDevice::GamepadAxis && Binding.Primary.Code == Binding.Secondary.Code))
						return std::unexpected(Error(BindingLocation + " Stick controls must be distinct."));
				}
			}
		}
		return {};
	}

	std::expected<GameplayActionMap, GameplayInputError> ParseGameplayActionMap(std::string_view JsonText)
	{
		if (JsonText.size() > MaximumJsonBytes)
			return std::unexpected(Error("Gameplay action map JSON exceeds the 1 MiB limit."));
		try
		{
			const Json Root = Json::parse(JsonText.begin(), JsonText.end());
			if (!Root.is_object()) return std::unexpected(Error("Gameplay action map root must be an object."));
			if (!Root.contains("version") ||
				(!Root["version"].is_number_unsigned() && !Root["version"].is_number_integer()))
				return std::unexpected(Error("Gameplay action map requires an integer version."));
			const int64_t Version = Root["version"].get<int64_t>();
			if (Version != 1) return std::unexpected(Error("Unsupported gameplay action map version."));
			if (!Root.contains("actions") || !Root["actions"].is_array())
				return std::unexpected(Error("Gameplay action map requires an actions array."));
			if (Root["actions"].size() > GameplayInputService::MaximumActions)
				return std::unexpected(Error("Gameplay action map exceeds the 256 action limit."));

			GameplayActionMap Map;
			Map.Version = 1;
			Map.Actions.reserve(Root["actions"].size());
			size_t ParsedBindingCount = 0;
			for (size_t ActionIndex = 0; ActionIndex < Root["actions"].size(); ++ActionIndex)
			{
				const Json& ActionJson = Root["actions"][ActionIndex];
				const std::string Location = DescribeIndex("actions", ActionIndex);
				if (!ActionJson.is_object()) return std::unexpected(Error(Location + " must be an object."));
				if (!ActionJson.contains("id") || !ActionJson["id"].is_string())
					return std::unexpected(Error(Location + ".id must be a string."));
				if (!ActionJson.contains("type") || !ActionJson["type"].is_string())
					return std::unexpected(Error(Location + ".type must be a string."));
				if (!ActionJson.contains("context") || !ActionJson["context"].is_string())
					return std::unexpected(Error(Location + ".context must be a string."));
				if (!ActionJson.contains("bindings") || !ActionJson["bindings"].is_array())
					return std::unexpected(Error(Location + ".bindings must be an array."));
				if (ActionJson["bindings"].size() > MaximumBindingsPerAction)
					return std::unexpected(Error(Location + ".bindings exceeds the 64 binding limit."));
				ParsedBindingCount += ActionJson["bindings"].size();
				if (ParsedBindingCount > MaximumBindingsTotal)
					return std::unexpected(Error("Gameplay action map exceeds the total binding limit."));

				GameplayActionDefinition Action;
				Action.Id = ActionJson["id"].get<std::string>();
				const auto Type = ParseActionType(ActionJson["type"].get<std::string>());
				if (!Type) return std::unexpected(Error(Location + ".type is not Digital, Axis1D or Axis2D."));
				Action.Type = *Type;
				const auto Context = ParseContext(ActionJson["context"].get<std::string>());
				if (!Context) return std::unexpected(Error(Location + ".context is unsupported."));
				Action.Context = *Context;
				Action.Bindings.reserve(ActionJson["bindings"].size());

				for (size_t BindingIndex = 0; BindingIndex < ActionJson["bindings"].size(); ++BindingIndex)
				{
					const Json& BindingJson = ActionJson["bindings"][BindingIndex];
					const std::string BindingLocation = Location + ".bindings[" + std::to_string(BindingIndex) + "]";
					if (!BindingJson.is_object()) return std::unexpected(Error(BindingLocation + " must be an object."));
					if (!BindingJson.contains("id") || !BindingJson["id"].is_string())
						return std::unexpected(Error(BindingLocation + ".id must be a string."));
					if (!BindingJson.contains("kind") || !BindingJson["kind"].is_string())
						return std::unexpected(Error(BindingLocation + ".kind must be a string."));
					if (!BindingJson.contains("primary") || !BindingJson["primary"].is_string())
						return std::unexpected(Error(BindingLocation + ".primary must be a named control string."));
					if (!BindingJson.contains("scaleX") || !BindingJson["scaleX"].is_number())
						return std::unexpected(Error(BindingLocation + ".scaleX must be numeric."));
					if (!BindingJson.contains("scaleY") || !BindingJson["scaleY"].is_number())
					return std::unexpected(Error(BindingLocation + ".scaleY must be numeric."));
					if (!BindingJson.contains("deadZone") || !BindingJson["deadZone"].is_number())
						return std::unexpected(Error(BindingLocation + ".deadZone must be numeric."));
					if (!BindingJson.contains("modifiers") || !BindingJson["modifiers"].is_array())
						return std::unexpected(Error(BindingLocation + ".modifiers must be an array."));
					if (BindingJson["modifiers"].size() > 4)
						return std::unexpected(Error(BindingLocation + ".modifiers exceeds the four modifier limit."));

					ActionBinding Binding;
					Binding.Id = BindingJson["id"].get<std::string>();
					const auto Kind = ParseBindingKind(BindingJson["kind"].get<std::string>());
					if (!Kind) return std::unexpected(Error(BindingLocation + ".kind is not Button, Axis or Stick."));
					Binding.Kind = *Kind;
					const auto Primary = ParseInputControlName(BindingJson["primary"].get<std::string>());
					if (!Primary) return std::unexpected(Error(BindingLocation + ".primary names an unsupported control."));
					Binding.Primary = *Primary;
					if (BindingJson.contains("secondary"))
					{
						if (!BindingJson["secondary"].is_string())
							return std::unexpected(Error(BindingLocation + ".secondary must be a named control string."));
						const auto Secondary = ParseInputControlName(BindingJson["secondary"].get<std::string>());
						if (!Secondary) return std::unexpected(Error(BindingLocation + ".secondary names an unsupported control."));
						Binding.Secondary = *Secondary;
					}
					Binding.ScaleX = BindingJson["scaleX"].get<float>();
					Binding.ScaleY = BindingJson["scaleY"].get<float>();
					Binding.DeadZone = BindingJson["deadZone"].get<float>();
					for (size_t ModifierIndex = 0; ModifierIndex < BindingJson["modifiers"].size(); ++ModifierIndex)
					{
						const Json& ModifierJson = BindingJson["modifiers"][ModifierIndex];
						if (!ModifierJson.is_string())
							return std::unexpected(Error(BindingLocation + ".modifiers entries must be named control strings."));
						const auto Modifier = ParseInputControlName(ModifierJson.get<std::string>());
						if (!Modifier) return std::unexpected(Error(BindingLocation + ".modifiers contains an unsupported control."));
						Binding.Modifiers[ModifierIndex] = *Modifier;
						++Binding.ModifierCount;
					}
					Action.Bindings.push_back(std::move(Binding));
				}
				Map.Actions.push_back(std::move(Action));
			}

			auto Validation = ValidateGameplayActionMap(Map);
			if (!Validation) return std::unexpected(Validation.error());
			return Map;
		}
		catch (const Json::exception& Exception)
		{
			return std::unexpected(Error(std::string("Could not parse gameplay action map: ") + Exception.what()));
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(Error(std::string("Could not parse gameplay action map: ") + Exception.what()));
		}
	}

	std::expected<std::string, GameplayInputError> SerializeGameplayActionMap(const GameplayActionMap& Map)
	{
		auto Validation = ValidateGameplayActionMap(Map);
		if (!Validation) return std::unexpected(Validation.error());
		try
		{
			Json Root;
			Root["version"] = Map.Version;
			Root["actions"] = Json::array();
			for (const GameplayActionDefinition& Action : Map.Actions)
			{
				Json ActionJson;
				ActionJson["id"] = Action.Id;
				ActionJson["type"] = ActionTypeName(Action.Type);
				ActionJson["context"] = ContextName(Action.Context);
				ActionJson["bindings"] = Json::array();
				for (uint8_t BindingIndex = 0; BindingIndex < Action.Bindings.size(); ++BindingIndex)
				{
					const ActionBinding& Binding = Action.Bindings[BindingIndex];
					Json BindingJson;
					BindingJson["id"] = Binding.Id;
					BindingJson["kind"] = BindingKindName(Binding.Kind);
					BindingJson["primary"] = std::string(GetInputControlName(Binding.Primary));
					if (Binding.Kind == ActionBindingKind::Stick)
						BindingJson["secondary"] = std::string(GetInputControlName(Binding.Secondary));
					BindingJson["scaleX"] = Binding.ScaleX;
					BindingJson["scaleY"] = Binding.ScaleY;
					BindingJson["deadZone"] = Binding.DeadZone;
					BindingJson["modifiers"] = Json::array();
					for (size_t ModifierIndex = 0; ModifierIndex < Binding.ModifierCount; ++ModifierIndex)
						BindingJson["modifiers"].push_back(std::string(GetInputControlName(Binding.Modifiers[ModifierIndex])));
					ActionJson["bindings"].push_back(std::move(BindingJson));
				}
				Root["actions"].push_back(std::move(ActionJson));
			}
			return Root.dump(2);
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(Error(std::string("Could not serialize gameplay action map: ") + Exception.what()));
		}
	}

	namespace
	{
		struct PhysicalKey
		{
			InputControl Control;
			InputWindowID Window = 0;
			uint64_t DeviceInstance = 0;

			bool operator==(const PhysicalKey&) const noexcept = default;
		};

		struct PhysicalKeyHash
		{
			size_t operator()(const PhysicalKey& Key) const noexcept
			{
				size_t Hash = static_cast<size_t>(Key.Control.Device);
				Hash = Hash * 1315423911u + Key.Control.Code;
				Hash = Hash * 1315423911u + static_cast<size_t>(Key.Window);
				Hash = Hash * 1315423911u + static_cast<size_t>(Key.DeviceInstance);
				return Hash;
			}
		};

		struct ActionState
		{
			bool Held = false;
			bool Pressed = false;
			bool Released = false;
			bool Canceled = false;
			bool PendingPressed = false;
			bool PendingReleased = false;
			bool PendingCanceled = false;
			float X = 0.0f;
			float Y = 0.0f;
		};
	}

	struct GameplayInputService::Impl
	{
		std::unordered_map<PhysicalKey, float, PhysicalKeyHash> Controls;
		std::unordered_set<PhysicalKey, PhysicalKeyHash> Quarantined;
		std::unordered_set<uint64_t> NewlyConnectedDevices;
		std::unordered_map<InputWindowID, std::pair<float, float>> PointerPositions;
		std::unordered_map<std::string, ActionState> Actions;
		InputRoutingState PreviousRouting;
		bool HasPreviousRouting = false;
		bool MapChangedPending = false;
		bool ResetPending = false;
	};

	namespace
	{
		[[nodiscard]] const InputContextActivation* FindActivation(const InputRoutingState& Routing, InputContext Context) noexcept
		{
			const size_t Count = (std::min)(Routing.ContextCount, Routing.Contexts.size());
			for (size_t Index = 0; Index < Count; ++Index)
				if (Routing.Contexts[Index].Active && Routing.Contexts[Index].Context == Context)
					return &Routing.Contexts[Index];
			return nullptr;
		}

		[[nodiscard]] bool IsGlobalGamepad(InputControl Control) noexcept
		{
			return Control.Device == InputDevice::GamepadAxis || Control.Device == InputDevice::GamepadButton;
		}

		[[nodiscard]] bool ContextOwnsControl(
			const InputContextActivation& Activation,
			const InputRoutingState& Routing,
			const PhysicalKey& Key) noexcept
		{
			if (IsGlobalGamepad(Key.Control))
			return Routing.FocusedWindow != 0 && Activation.OwnerWindow != 0 &&
				Activation.OwnerWindow == Routing.FocusedWindow;
			return Key.Window != 0 && Key.Window == Routing.FocusedWindow &&
				Activation.OwnerWindow == Key.Window;
		}

		[[nodiscard]] bool ContextCanReceive(
			InputContext Context,
			const InputRoutingState& Routing,
			const PhysicalKey& Key) noexcept
		{
			const InputContextActivation* OwnActivation = FindActivation(Routing, Context);
			if (!OwnActivation || !ContextOwnsControl(*OwnActivation, Routing, Key)) return false;

			const int Priority = static_cast<int>(Context);
			const InputDeviceMask Mask = DeviceMask(Key.Control.Device);
			const size_t Count = (std::min)(Routing.ContextCount, Routing.Contexts.size());
			for (size_t Index = 0; Index < Count; ++Index)
			{
				const InputContextActivation& Higher = Routing.Contexts[Index];
				if (!Higher.Active || !IsContextValid(Higher.Context) || !IsDeviceMaskValid(Higher.Consumes) ||
					static_cast<int>(Higher.Context) <= Priority ||
					!HasInputDevice(Higher.Consumes, Mask)) continue;
				if (ContextOwnsControl(Higher, Routing, Key)) return false;
			}
			return true;
		}

		[[nodiscard]] bool IsQuarantined(
			const std::unordered_set<PhysicalKey, PhysicalKeyHash>& Quarantined,
			const PhysicalKey& Key) noexcept
		{
			return Quarantined.contains(Key);
		}

		[[nodiscard]] bool ModifiersHeld(
			const ActionBinding& Binding,
			const PhysicalKey& PrimaryKey,
			InputContext Context,
			const InputRoutingState& Routing,
			const std::unordered_map<PhysicalKey, float, PhysicalKeyHash>& Controls,
			const std::unordered_set<PhysicalKey, PhysicalKeyHash>& Quarantined) noexcept;

		[[nodiscard]] float ReadAxis(
			const std::unordered_map<PhysicalKey, float, PhysicalKeyHash>& Controls,
			const std::unordered_set<PhysicalKey, PhysicalKeyHash>& Quarantined,
			const ActionBinding& Binding,
			InputContext Context,
			const InputRoutingState& Routing,
			uint64_t Instance,
			float DeadZone) noexcept
		{
			float Sum = 0.0f;
			for (const auto& [Key, Value] : Controls)
			{
				if (Key.Control != Binding.Primary || IsQuarantined(Quarantined, Key) ||
					(Instance != 0 && Key.DeviceInstance != Instance)) continue;
				if (IsGlobalGamepad(Binding.Primary))
				{
					if (!ContextCanReceive(Context, Routing, Key)) continue;
				}
				else if (!ContextCanReceive(Context, Routing, Key)) continue;
				if (!ModifiersHeld(Binding, Key, Context, Routing, Controls, Quarantined)) continue;

				float Adjusted = IsDigitalControl(Binding.Primary) ? (Value > ActionActivationEpsilon ? 1.0f : 0.0f) : Value;
				const float Magnitude = std::abs(Adjusted);
				if (DeadZone > 0.0f && Magnitude <= DeadZone) Adjusted = 0.0f;
				else if (DeadZone > 0.0f)
					Adjusted = std::copysign((Magnitude - DeadZone) / (1.0f - DeadZone), Adjusted);
				Sum += Adjusted;
			}
			return Sum;
		}

		[[nodiscard]] bool ModifiersHeld(
			const ActionBinding& Binding,
			const PhysicalKey& PrimaryKey,
			InputContext Context,
			const InputRoutingState& Routing,
			const std::unordered_map<PhysicalKey, float, PhysicalKeyHash>& Controls,
			const std::unordered_set<PhysicalKey, PhysicalKeyHash>& Quarantined) noexcept
		{
			for (size_t Index = 0; Index < Binding.ModifierCount; ++Index)
			{
				const InputControl Modifier = Binding.Modifiers[Index];
				bool Found = false;
				for (const auto& [Key, Value] : Controls)
				{
					if (Key.Control != Modifier || Value <= ActionActivationEpsilon || IsQuarantined(Quarantined, Key)) continue;
					if (IsGlobalGamepad(Modifier) != IsGlobalGamepad(PrimaryKey.Control)) continue;
					if (IsGlobalGamepad(Modifier))
					{
					if (Key.DeviceInstance != PrimaryKey.DeviceInstance || !ContextCanReceive(Context, Routing, Key)) continue;
					}
					else if (Key.Window != PrimaryKey.Window || !ContextCanReceive(Context, Routing, Key)) continue;
					Found = true;
					break;
				}
				if (!Found) return false;
			}
			return true;
		}

		[[nodiscard]] bool AnyButtonBinding(
			const GameplayActionDefinition& Action,
			const ActionBinding& Binding,
			const InputRoutingState& Routing,
			const std::unordered_map<PhysicalKey, float, PhysicalKeyHash>& Controls,
			const std::unordered_set<PhysicalKey, PhysicalKeyHash>& Quarantined) noexcept
		{
			for (const auto& [Key, Value] : Controls)
			{
				if (Key.Control != Binding.Primary || Value <= ActionActivationEpsilon || IsQuarantined(Quarantined, Key) ||
					!ContextCanReceive(Action.Context, Routing, Key)) continue;
				if (ModifiersHeld(Binding, Key, Action.Context, Routing, Controls, Quarantined)) return true;
			}
			return false;
		}

		[[nodiscard]] std::pair<float, float> ReadStick(
			const ActionBinding& Binding,
			InputContext Context,
			const InputRoutingState& Routing,
			const std::unordered_map<PhysicalKey, float, PhysicalKeyHash>& Controls,
			const std::unordered_set<PhysicalKey, PhysicalKeyHash>& Quarantined) noexcept
		{
			float SumX = 0.0f;
			float SumY = 0.0f;
			for (const auto& [Key, Value] : Controls)
			{
				if (Key.Control != Binding.Primary || IsQuarantined(Quarantined, Key) ||
					!ContextCanReceive(Context, Routing, Key)) continue;
				const PhysicalKey Secondary{ Binding.Secondary, Key.Window, Key.DeviceInstance };
				const auto SecondaryIt = Controls.find(Secondary);
				if (SecondaryIt == Controls.end() || IsQuarantined(Quarantined, Secondary) ||
					!ContextCanReceive(Context, Routing, Secondary)) continue;
				if (!ModifiersHeld(Binding, Key, Context, Routing, Controls, Quarantined)) continue;

				float X = ClampFinite(Value, -1.0f, 1.0f);
				float Y = ClampFinite(SecondaryIt->second, -1.0f, 1.0f);
				const float Magnitude = std::sqrt(X * X + Y * Y);
				if (Magnitude <= Binding.DeadZone) continue;
				const float ScaledMagnitude = (std::min)(Magnitude, 1.0f);
				const float RemappedMagnitude = Binding.DeadZone > 0.0f
					? (ScaledMagnitude - Binding.DeadZone) / (1.0f - Binding.DeadZone)
					: ScaledMagnitude;
				const float Scale = Magnitude > 0.0f ? RemappedMagnitude / Magnitude : 0.0f;
				SumX += X * Scale * Binding.ScaleX;
				SumY += Y * Scale * Binding.ScaleY;
			}
			return { SumX, SumY };
		}

		[[nodiscard]] ActionState EvaluateAction(
			const GameplayActionDefinition& Action,
			const InputRoutingState& Routing,
			const std::unordered_map<PhysicalKey, float, PhysicalKeyHash>& Controls,
			const std::unordered_set<PhysicalKey, PhysicalKeyHash>& Quarantined) noexcept
		{
			ActionState Result;
			if (!IsActionContextActive(Routing, Action.Context)) return Result;

			if (Action.Type == ActionValueType::Digital)
			{
				for (const ActionBinding& Binding : Action.Bindings)
					if (AnyButtonBinding(Action, Binding, Routing, Controls, Quarantined))
					{
						Result.Held = true;
						break;
					}
				Result.X = Result.Held ? 1.0f : 0.0f;
				return Result;
			}

			for (const ActionBinding& Binding : Action.Bindings)
			{
				if (Binding.Kind == ActionBindingKind::Stick)
				{
					const auto [X, Y] = ReadStick(Binding, Action.Context, Routing, Controls, Quarantined);
					Result.X += X;
					Result.Y += Y;
					continue;
				}

				float Value = 0.0f;
				if (IsDigitalControl(Binding.Primary))
				{
					for (const auto& [Key, Down] : Controls)
					{
						if (Key.Control != Binding.Primary || Down <= ActionActivationEpsilon ||
							IsQuarantined(Quarantined, Key) || !ContextCanReceive(Action.Context, Routing, Key) ||
							!ModifiersHeld(Binding, Key, Action.Context, Routing, Controls, Quarantined)) continue;
						Value += 1.0f;
					}
				}
				else
					Value = ReadAxis(Controls, Quarantined, Binding, Action.Context, Routing, 0, Binding.DeadZone);

				Result.X += Value * Binding.ScaleX;
				Result.Y += Value * Binding.ScaleY;
			}

			if (Action.Type == ActionValueType::Axis1D)
			{
				Result.X = ClampFinite(Result.X, -1.0f, 1.0f);
				Result.Y = 0.0f;
				Result.Held = std::abs(Result.X) > ActionActivationEpsilon;
			}
			else
			{
				Result.X = ClampFinite(Result.X, -1.0f, 1.0f);
				Result.Y = ClampFinite(Result.Y, -1.0f, 1.0f);
				const float Magnitude = std::sqrt(Result.X * Result.X + Result.Y * Result.Y);
				if (Magnitude > 1.0f)
				{
					Result.X /= Magnitude;
					Result.Y /= Magnitude;
				}
				Result.Held = std::abs(Result.X) > ActionActivationEpsilon || std::abs(Result.Y) > ActionActivationEpsilon;
			}
			return Result;
		}

		[[nodiscard]] std::vector<ActionSnapshotEntry> EvaluateAll(
			const GameplayActionMap& Map,
			const InputRoutingState& Routing,
			const std::unordered_map<PhysicalKey, float, PhysicalKeyHash>& Controls,
			const std::unordered_set<PhysicalKey, PhysicalKeyHash>& Quarantined)
		{
			std::vector<ActionSnapshotEntry> Results;
			Results.reserve(Map.Actions.size());
			for (const GameplayActionDefinition& Action : Map.Actions)
			{
				ActionSnapshotEntry Entry;
				Entry.Id = Action.Id;
				const ActionState State = EvaluateAction(Action, Routing, Controls, Quarantined);
				Entry.Value.Type = Action.Type;
				Entry.Value.Held = State.Held;
				Entry.Value.X = State.X;
				Entry.Value.Y = State.Y;
				Results.push_back(std::move(Entry));
			}
			return Results;
		}

		[[nodiscard]] const ActionState* FindActionState(
			const std::unordered_map<std::string, ActionState>& States,
			std::string_view Id) noexcept
		{
			for (const auto& [Candidate, State] : States)
				if (Candidate == Id) return &State;
			return nullptr;
		}

		void RecordTransitions(
			std::unordered_map<std::string, ActionState>& Previous,
			const std::vector<ActionSnapshotEntry>& Current,
			bool Cancellation)
		{
			for (const ActionSnapshotEntry& Entry : Current)
			{
				ActionState& State = Previous[Entry.Id];
				const bool WasHeld = State.Held;
				const bool IsHeldNow = Entry.Value.Held;
				if (!WasHeld && IsHeldNow) State.Pressed = true;
				if (WasHeld && !IsHeldNow)
				{
					State.Released = true;
					if (Cancellation) State.Canceled = true;
				}
				if (!WasHeld && IsHeldNow) State.PendingPressed = true;
				if (WasHeld && !IsHeldNow)
				{
					State.PendingReleased = true;
					if (Cancellation) State.PendingCanceled = true;
				}
				State.Held = IsHeldNow;
				State.X = Entry.Value.X;
				State.Y = Entry.Value.Y;
			}
		}

		void QuarantineRoutingChanges(
			std::unordered_set<PhysicalKey, PhysicalKeyHash>& Quarantined,
			const std::unordered_map<PhysicalKey, float, PhysicalKeyHash>& Controls,
			const InputRoutingState& Previous,
			const InputRoutingState& Current)
		{
			for (const auto& [Key, Value] : Controls)
			{
				if (std::abs(Value) <= ActionActivationEpsilon) continue;
				bool EligibilityChanged = false;
				for (uint8_t ContextValue = 0; ContextValue <= static_cast<uint8_t>(InputContext::RuntimeUI); ++ContextValue)
				{
					const auto Context = static_cast<InputContext>(ContextValue);
					if (ContextCanReceive(Context, Previous, Key) != ContextCanReceive(Context, Current, Key))
					{
						EligibilityChanged = true;
						break;
					}
				}
				if (EligibilityChanged)
					Quarantined.insert(Key);
			}
		}

		void EraseMatching(
			std::unordered_map<PhysicalKey, float, PhysicalKeyHash>& Controls,
			std::unordered_set<PhysicalKey, PhysicalKeyHash>& Quarantined,
			const auto& Predicate,
			bool Quarantine)
		{
			for (auto It = Controls.begin(); It != Controls.end();)
			{
				if (Predicate(It->first))
				{
					if (Quarantine && IsDigitalControl(It->first.Control)) Quarantined.insert(It->first);
					It = Controls.erase(It);
				}
				else ++It;
			}
		}

		[[nodiscard]] bool IsEventControlValid(const InputEvent& Event) noexcept
		{
			switch (Event.Kind)
			{
			case InputEventKind::FocusLost:
			case InputEventKind::WindowDestroyed:
			case InputEventKind::CaptureCancelled: return Event.Window != 0;
			case InputEventKind::DeviceConnected:
			case InputEventKind::DeviceDisconnected: return Event.DeviceInstance != 0;
			case InputEventKind::Pressed:
			case InputEventKind::Released:
				return IsValidControl(Event.Control) && IsDigitalControl(Event.Control);
			case InputEventKind::Value:
				return IsValidControl(Event.Control) && (IsDigitalControl(Event.Control) || IsAxisControl(Event.Control));
			case InputEventKind::PointerMoved:
			case InputEventKind::Scrolled:
				return IsValidControl(Event.Control) && IsAxisControl(Event.Control);
			}
			return false;
		}

		void ApplyEvent(
			const InputEvent& Event,
			std::unordered_map<PhysicalKey, float, PhysicalKeyHash>& Controls,
			std::unordered_set<PhysicalKey, PhysicalKeyHash>& Quarantined,
			std::unordered_map<InputWindowID, std::pair<float, float>>& PointerPositions,
			std::unordered_set<uint64_t>& NewlyConnectedDevices)
		{
			const auto AddDelta = [&Controls, &Event](InputControl Control, float Delta)
			{
				const PhysicalKey Key{ Control, Event.Window, Event.DeviceInstance };
				Controls[Key] += Delta;
			};
			switch (Event.Kind)
			{
			case InputEventKind::Pressed:
			{
				const PhysicalKey Key{ Event.Control, Event.Window, Event.DeviceInstance };
				if (IsGlobalGamepad(Event.Control) && NewlyConnectedDevices.contains(Event.DeviceInstance))
					Quarantined.insert(Key);
				if (!Quarantined.contains(Key)) Controls[Key] = 1.0f;
				break;
			}
			case InputEventKind::Released:
			{
				const PhysicalKey Key{ Event.Control, Event.Window, Event.DeviceInstance };
				Controls.erase(Key);
				Quarantined.erase(Key);
				break;
			}
			case InputEventKind::Value:
			{
				if (!std::isfinite(Event.X)) break;
				const PhysicalKey Key{ Event.Control, Event.Window, Event.DeviceInstance };
				float Value = Event.X;
				if (Event.Control.Device == InputDevice::GamepadAxis && Event.Control.Code >= static_cast<uint16_t>(GamepadAxis::LeftTrigger))
					Value = ClampFinite(Value, 0.0f, 1.0f);
				else if (IsAxisControl(Event.Control)) Value = ClampFinite(Value, -1.0f, 1.0f);
				else Value = Value > ActionActivationEpsilon ? 1.0f : 0.0f;
				if (IsGlobalGamepad(Event.Control) && NewlyConnectedDevices.contains(Event.DeviceInstance) &&
					std::abs(Value) > ActionActivationEpsilon)
					Quarantined.insert(Key);
				if (Quarantined.contains(Key))
				{
					Controls.erase(Key);
					if (std::abs(Value) <= ActionActivationEpsilon) Quarantined.erase(Key);
					break;
				}
				Controls[Key] = Value;
				break;
			}
			case InputEventKind::PointerMoved:
			{
				if (!std::isfinite(Event.X) || !std::isfinite(Event.Y)) break;
				auto It = PointerPositions.find(Event.Window);
				if (It != PointerPositions.end())
				{
					const float DeltaX = ClampFinite(Event.X - It->second.first, -1.0e6f, 1.0e6f);
					const float DeltaY = ClampFinite(Event.Y - It->second.second, -1.0e6f, 1.0e6f);
					AddDelta(MouseMotionControl(MouseControl::MotionX), DeltaX);
					AddDelta(MouseMotionControl(MouseControl::MotionY), DeltaY);
				}
				PointerPositions[Event.Window] = { Event.X, Event.Y };
				break;
			}
			case InputEventKind::Scrolled:
				if (std::isfinite(Event.X) && std::isfinite(Event.Y))
				{
					AddDelta(MouseWheelControl(MouseControl::WheelX), Event.X);
					AddDelta(MouseWheelControl(MouseControl::WheelY), Event.Y);
				}
				break;
			case InputEventKind::FocusLost:
			case InputEventKind::WindowDestroyed:
				EraseMatching(Controls, Quarantined, [&Event](const PhysicalKey& Key) {
					return Key.Window == Event.Window && !IsGlobalGamepad(Key.Control);
				}, true);
				PointerPositions.erase(Event.Window);
				if (Event.Kind == InputEventKind::WindowDestroyed)
					for (auto It = Quarantined.begin(); It != Quarantined.end();)
						if (It->Window == Event.Window && !IsGlobalGamepad(It->Control)) It = Quarantined.erase(It);
						else ++It;
				break;
			case InputEventKind::CaptureCancelled:
				EraseMatching(Controls, Quarantined, [&Event](const PhysicalKey& Key) {
					return Key.Window == Event.Window &&
						(Key.Control.Device == InputDevice::MouseButton || Key.Control.Device == InputDevice::MouseMotion ||
						 Key.Control.Device == InputDevice::MouseWheel);
				}, true);
				PointerPositions.erase(Event.Window);
				break;
			case InputEventKind::DeviceDisconnected:
				EraseMatching(Controls, Quarantined, [&Event](const PhysicalKey& Key) {
					return IsGlobalGamepad(Key.Control) && Key.DeviceInstance == Event.DeviceInstance;
				}, false);
				NewlyConnectedDevices.erase(Event.DeviceInstance);
				for (auto It = Quarantined.begin(); It != Quarantined.end();)
					if (IsGlobalGamepad(It->Control) && It->DeviceInstance == Event.DeviceInstance)
						It = Quarantined.erase(It);
					else ++It;
				break;
			case InputEventKind::DeviceConnected:
				NewlyConnectedDevices.insert(Event.DeviceInstance);
				break;
			}
		}

		template<typename TImpl>
		void RecordEventActionTransitions(
			TImpl& Impl,
			const GameplayActionMap& Map,
			const InputRoutingState& Routing,
			bool Cancellation)
		{
			const auto Current = EvaluateAll(Map, Routing, Impl.Controls, Impl.Quarantined);
			RecordTransitions(Impl.Actions, Current, Cancellation);
		}
	} // namespace

	GameplayInputService::GameplayInputService() : m_Impl(std::make_unique<Impl>())
	{
		m_Events.reserve(MaximumQueuedEvents);
		m_Snapshot.Actions.reserve(MaximumActions);
		m_PendingSimulationEdges.Actions.reserve(MaximumActions);
	}

	GameplayInputService::~GameplayInputService() = default;

	GameplayInputService::GameplayInputService(GameplayInputService&&) noexcept = default;
	GameplayInputService& GameplayInputService::operator=(GameplayInputService&&) noexcept = default;

	std::expected<void, GameplayInputError> GameplayInputService::SetActionMap(GameplayActionMap Map)
	{
		try
		{
			auto Validation = ValidateGameplayActionMap(Map);
			if (!Validation) return Validation;
			auto NewActions = std::unordered_map<std::string, ActionState>{};
			NewActions.reserve(Map.Actions.size());
			auto NewPending = std::vector<ActionSnapshotEntry>{};
			NewPending.reserve(Map.Actions.size());
			auto NewQuarantined = m_Impl->Quarantined;
			for (const auto& [Key, Value] : m_Impl->Controls)
				if (std::abs(Value) > ActionActivationEpsilon) NewQuarantined.insert(Key);

			for (const GameplayActionDefinition& Action : Map.Actions)
			{
				ActionState State;
				if (auto Previous = m_Impl->Actions.find(Action.Id); Previous != m_Impl->Actions.end())
					State.Held = Previous->second.Held;
				NewActions.emplace(Action.Id, State);
				NewPending.push_back({ Action.Id, ActionValue{ .Type = Action.Type } });
			}

			m_Map = std::move(Map);
			m_Impl->Quarantined.swap(NewQuarantined);
			m_Impl->Actions.swap(NewActions);
			m_Impl->MapChangedPending = true;
			m_PendingSimulationEdges.Actions.swap(NewPending);
			m_PendingSimulationEdges.Frame = m_Frame;
			return {};
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(Error(std::string("Could not install gameplay action map: ") + Exception.what()));
		}
	}

	bool GameplayInputService::QueueEvent(const InputEvent& Event) noexcept
	{
		if (!IsEventControlValid(Event)) return false;
		if ((Event.Kind == InputEventKind::Value && (!std::isfinite(Event.X) || !std::isfinite(Event.Y))) ||
			((Event.Kind == InputEventKind::PointerMoved || Event.Kind == InputEventKind::Scrolled) &&
				(!std::isfinite(Event.X) || !std::isfinite(Event.Y)))) return false;
		if ((Event.Kind == InputEventKind::Pressed || Event.Kind == InputEventKind::Released ||
			Event.Kind == InputEventKind::Value || Event.Kind == InputEventKind::PointerMoved ||
			Event.Kind == InputEventKind::Scrolled) && Event.Window == 0 && !IsGlobalGamepad(Event.Control)) return false;
		if ((Event.Kind == InputEventKind::DeviceConnected || Event.Kind == InputEventKind::DeviceDisconnected) &&
			Event.DeviceInstance == 0) return false;
		if ((Event.Kind == InputEventKind::Pressed || Event.Kind == InputEventKind::Released || Event.Kind == InputEventKind::Value) &&
			IsGlobalGamepad(Event.Control) && Event.DeviceInstance == 0) return false;
		if (m_Events.size() >= MaximumQueuedEvents)
		{
			m_EventOverflowed = true;
			m_EventOverflowPending = true;
			return false;
		}
		try
		{
			m_Events.push_back(Event);
			return true;
		}
		catch (...)
		{
			m_EventOverflowed = true;
			m_EventOverflowPending = true;
			return false;
		}
	}

	void GameplayInputService::PublishFrame(const InputRoutingState& Routing) noexcept
	{
		try
		{
			++m_Frame;
			m_Impl->NewlyConnectedDevices.clear();
			for (auto& [Id, State] : m_Impl->Actions)
			{
				State.Pressed = false;
				State.Released = false;
				State.Canceled = false;
			}
			if (m_EventOverflowPending)
			{
				m_EventOverflowPending = false;
				for (const auto& [Key, Value] : m_Impl->Controls)
					if (std::abs(Value) > ActionActivationEpsilon) m_Impl->Quarantined.insert(Key);
				m_Impl->Controls.clear();
				m_Impl->PointerPositions.clear();
				m_Events.clear();
				m_Impl->NewlyConnectedDevices.clear();
				m_Snapshot.Frame = m_Frame;
				m_Snapshot.Actions.clear();
				for (const GameplayActionDefinition& Action : m_Map.Actions)
				{
					ActionState& State = m_Impl->Actions[Action.Id];
					const bool WasHeld = State.Held;
					State.Held = false;
					State.X = 0.0f;
					State.Y = 0.0f;
					State.Released = WasHeld;
					State.Canceled = WasHeld;
					State.PendingReleased = State.PendingReleased || WasHeld;
					State.PendingCanceled = State.PendingCanceled || WasHeld;
					ActionValue Value{ .Type = Action.Type, .Released = WasHeld, .Canceled = WasHeld };
					m_Snapshot.Actions.push_back({ Action.Id, Value });
					auto Pending = std::find_if(m_PendingSimulationEdges.Actions.begin(), m_PendingSimulationEdges.Actions.end(),
						[&Action](const ActionSnapshotEntry& Candidate) { return Candidate.Id == Action.Id; });
					if (Pending != m_PendingSimulationEdges.Actions.end())
					{
						Pending->Value.Released = Pending->Value.Released || WasHeld;
						Pending->Value.Canceled = Pending->Value.Canceled || WasHeld;
						Pending->Value.Held = false;
						Pending->Value.X = 0.0f;
						Pending->Value.Y = 0.0f;
					}
				}
				m_PendingSimulationEdges.Frame = m_Frame;
				m_Impl->PreviousRouting = Routing;
				m_Impl->HasPreviousRouting = true;
				m_Impl->MapChangedPending = false;
				m_Impl->ResetPending = false;
				return;
			}
			// Motion and wheel values are frame-local. Button and gamepad states persist until release.
			for (auto It = m_Impl->Controls.begin(); It != m_Impl->Controls.end();)
				if (It->first.Control.Device == InputDevice::MouseMotion || It->first.Control.Device == InputDevice::MouseWheel)
					It = m_Impl->Controls.erase(It);
				else ++It;

			if (m_Impl->HasPreviousRouting)
			{
				const auto Before = EvaluateAll(m_Map, m_Impl->PreviousRouting, m_Impl->Controls, m_Impl->Quarantined);
				QuarantineRoutingChanges(m_Impl->Quarantined, m_Impl->Controls, m_Impl->PreviousRouting, Routing);
				const auto AfterQuarantine = EvaluateAll(m_Map, Routing, m_Impl->Controls, m_Impl->Quarantined);
				RecordTransitions(m_Impl->Actions, Before, m_Impl->MapChangedPending);
				RecordTransitions(m_Impl->Actions, AfterQuarantine, true);
			}
			if (m_Impl->ResetPending)
			{
				for (auto& [Id, State] : m_Impl->Actions)
				{
					if (!State.Held) continue;
					State.Held = false;
					State.X = 0.0f;
					State.Y = 0.0f;
					State.Released = true;
					State.Canceled = true;
					State.PendingReleased = true;
					State.PendingCanceled = true;
				}
				m_Impl->ResetPending = false;
			}

			for (const InputEvent& Event : m_Events)
			{
				ApplyEvent(Event, m_Impl->Controls, m_Impl->Quarantined, m_Impl->PointerPositions,
					m_Impl->NewlyConnectedDevices);
				const bool IsCancellation = Event.Kind == InputEventKind::FocusLost ||
					Event.Kind == InputEventKind::WindowDestroyed || Event.Kind == InputEventKind::CaptureCancelled ||
					Event.Kind == InputEventKind::DeviceDisconnected;
				RecordEventActionTransitions(*m_Impl, m_Map, Routing, IsCancellation);
			}
			m_Events.clear();

			const auto Current = EvaluateAll(m_Map, Routing, m_Impl->Controls, m_Impl->Quarantined);
			RecordTransitions(m_Impl->Actions, Current, false);
			m_Snapshot.Frame = m_Frame;
			m_Snapshot.Actions.clear();
			m_Snapshot.Actions.reserve(Current.size());
			for (const ActionSnapshotEntry& Entry : Current)
			{
				ActionSnapshotEntry Published = Entry;
				if (const ActionState* State = FindActionState(m_Impl->Actions, Entry.Id))
				{
					Published.Value.Pressed = State->Pressed;
					Published.Value.Released = State->Released;
					Published.Value.Canceled = State->Canceled;
					auto Pending = std::find_if(m_PendingSimulationEdges.Actions.begin(), m_PendingSimulationEdges.Actions.end(),
						[&Entry](const ActionSnapshotEntry& Candidate) { return Candidate.Id == Entry.Id; });
					if (Pending == m_PendingSimulationEdges.Actions.end())
					{
						m_PendingSimulationEdges.Actions.push_back({ Entry.Id, ActionValue{ .Type = Entry.Value.Type } });
						Pending = std::prev(m_PendingSimulationEdges.Actions.end());
					}
					Pending->Value.Type = Entry.Value.Type;
					Pending->Value.Pressed = Pending->Value.Pressed || State->PendingPressed;
					Pending->Value.Released = Pending->Value.Released || State->PendingReleased;
					Pending->Value.Canceled = Pending->Value.Canceled || State->PendingCanceled;
					Pending->Value.Held = Entry.Value.Held;
					Pending->Value.X = Entry.Value.X;
					Pending->Value.Y = Entry.Value.Y;
				}
				m_Snapshot.Actions.push_back(std::move(Published));
			}
			m_PendingSimulationEdges.Frame = m_Frame;
			m_Impl->PreviousRouting = Routing;
			m_Impl->HasPreviousRouting = true;
			m_Impl->MapChangedPending = false;
			m_Impl->ResetPending = false;
		}
		catch (...)
		{
			m_EventOverflowed = true;
			m_EventOverflowPending = true;
			m_Events.clear();
		}
	}

	void GameplayInputService::Reset() noexcept
	{
		m_Events.clear();
		try
		{
			for (const auto& [Key, Value] : m_Impl->Controls)
				if (std::abs(Value) > ActionActivationEpsilon) m_Impl->Quarantined.insert(Key);
			m_Impl->Controls.clear();
			m_Impl->NewlyConnectedDevices.clear();
			m_Impl->PointerPositions.clear();
			m_Impl->HasPreviousRouting = false;
			m_Impl->PreviousRouting = {};
			m_Impl->ResetPending = true;
			m_Impl->MapChangedPending = false;
			for (ActionSnapshotEntry& Entry : m_PendingSimulationEdges.Actions)
			{
				Entry.Value.Pressed = false;
				Entry.Value.Released = false;
				Entry.Value.Canceled = false;
				Entry.Value.Held = false;
				Entry.Value.X = 0.0f;
				Entry.Value.Y = 0.0f;
			}
			for (auto& [Id, State] : m_Impl->Actions)
			{
				State.Pressed = false;
				State.Released = false;
				State.Canceled = false;
				State.PendingPressed = false;
				State.PendingReleased = false;
				State.PendingCanceled = false;
			}
			m_EventOverflowed = false;
			m_EventOverflowPending = false;
		}
		catch (...)
		{
			m_EventOverflowed = true;
			m_EventOverflowPending = true;
		}
	}

	SimulationActionSnapshot GameplayInputService::BeginSimulationStep()
	{
		SimulationActionSnapshot Result;
		Result.Frame = m_PendingSimulationEdges.Frame;
		Result.Actions = m_PendingSimulationEdges.Actions;
		for (ActionSnapshotEntry& Entry : m_PendingSimulationEdges.Actions)
		{
			Entry.Value.Pressed = false;
			Entry.Value.Released = false;
			Entry.Value.Canceled = false;
			if (const ActionValue* Current = m_Snapshot.Find(Entry.Id))
			{
				Entry.Value.Held = Current->Held;
				Entry.Value.X = Current->X;
				Entry.Value.Y = Current->Y;
			}
		}
		for (auto& [Id, State] : m_Impl->Actions)
		{
					State.PendingPressed = false;
					State.PendingReleased = false;
					State.PendingCanceled = false;
		}
		return Result;
	}

	const ActionValue* ActionSnapshot::Find(std::string_view Id) const noexcept
	{
		for (const ActionSnapshotEntry& Entry : Actions)
			if (Entry.Id == Id) return &Entry.Value;
		return nullptr;
	}
}
