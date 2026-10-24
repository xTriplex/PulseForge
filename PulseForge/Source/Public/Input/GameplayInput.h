#pragma once

#include "Core/Core.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace PulseForge
{
	using InputWindowID = uint64_t;

	enum class InputDevice : uint8_t
	{
		Keyboard,
		MouseButton,
		MouseMotion,
		MouseWheel,
		GamepadButton,
		GamepadAxis
	};

	enum class InputKey : uint16_t
	{
		Space = 32,
		Apostrophe = 39,
		Comma = 44,
		Minus = 45,
		Period = 46,
		Slash = 47,
		Digit0 = 48,
		Digit1 = 49,
		Digit2 = 50,
		Digit3 = 51,
		Digit4 = 52,
		Digit5 = 53,
		Digit6 = 54,
		Digit7 = 55,
		Digit8 = 56,
		Digit9 = 57,
		Semicolon = 59,
		Equal = 61,
		A = 65,
		B = 66,
		C = 67,
		D = 68,
		E = 69,
		F = 70,
		G = 71,
		H = 72,
		I = 73,
		J = 74,
		K = 75,
		L = 76,
		M = 77,
		N = 78,
		O = 79,
		P = 80,
		Q = 81,
		R = 82,
		S = 83,
		T = 84,
		U = 85,
		V = 86,
		W = 87,
		X = 88,
		Y = 89,
		Z = 90,
		LeftBracket = 91,
		Backslash = 92,
		RightBracket = 93,
		GraveAccent = 96,
		Escape = 256,
		Enter = 257,
		Tab = 258,
		Backspace = 259,
		Insert = 260,
		Delete = 261,
		Right = 262,
		Left = 263,
		Down = 264,
		Up = 265,
		PageUp = 266,
		PageDown = 267,
		Home = 268,
		End = 269,
		CapsLock = 280,
		ScrollLock = 281,
		NumLock = 282,
		PrintScreen = 283,
		Pause = 284,
		F1 = 290,
		F2 = 291,
		F3 = 292,
		F4 = 293,
		F5 = 294,
		F6 = 295,
		F7 = 296,
		F8 = 297,
		F9 = 298,
		F10 = 299,
		F11 = 300,
		F12 = 301,
		F13 = 302,
		F14 = 303,
		F15 = 304,
		F16 = 305,
		F17 = 306,
		F18 = 307,
		F19 = 308,
		F20 = 309,
		F21 = 310,
		F22 = 311,
		F23 = 312,
		F24 = 313,
		F25 = 314,
		Keypad0 = 320,
		Keypad1 = 321,
		Keypad2 = 322,
		Keypad3 = 323,
		Keypad4 = 324,
		Keypad5 = 325,
		Keypad6 = 326,
		Keypad7 = 327,
		Keypad8 = 328,
		Keypad9 = 329,
		KeypadDecimal = 330,
		KeypadDivide = 331,
		KeypadMultiply = 332,
		KeypadSubtract = 333,
		KeypadAdd = 334,
		KeypadEnter = 335,
		KeypadEqual = 336,
		LeftShift = 340,
		LeftControl = 341,
		LeftAlt = 342,
		LeftSuper = 343,
		RightShift = 344,
		RightControl = 345,
		RightAlt = 346,
		RightSuper = 347,
		Menu = 348
	};

	enum class MouseControl : uint16_t
	{
		Left = 0,
		Right = 1,
		Middle = 2,
		Button4 = 3,
		Button5 = 4,
		Button6 = 5,
		Button7 = 6,
		Button8 = 7,
		MotionX = 0,
		MotionY = 1,
		WheelX = 0,
		WheelY = 1
	};

	enum class GamepadButton : uint16_t
	{
		A = 0,
		B = 1,
		X = 2,
		Y = 3,
		LeftBumper = 4,
		RightBumper = 5,
		Back = 6,
		Start = 7,
		Guide = 8,
		LeftThumb = 9,
		RightThumb = 10,
		DpadUp = 11,
		DpadRight = 12,
		DpadDown = 13,
		DpadLeft = 14
	};

	enum class GamepadAxis : uint16_t
	{
		LeftX = 0,
		LeftY = 1,
		RightX = 2,
		RightY = 3,
		LeftTrigger = 4,
		RightTrigger = 5
	};

	// Keyboard codes are engine-owned names mapped from GLFW key tokens. Platform scancodes and text glyphs are not persisted.
	struct InputControl
	{
		InputDevice Device = InputDevice::Keyboard;
		uint16_t Code = 0;

		[[nodiscard]] bool operator==(const InputControl&) const noexcept = default;
	};

	[[nodiscard]] PULSEFORGE_API std::string_view GetInputControlName(InputControl Control) noexcept;
	[[nodiscard]] PULSEFORGE_API std::optional<InputControl> ParseInputControlName(std::string_view Name) noexcept;

	enum class InputEventKind : uint8_t
	{
		Pressed,
		Released,
		Value,
		PointerMoved,
		Scrolled,
		FocusLost,
		WindowDestroyed,
		CaptureCancelled,
		DeviceConnected,
		DeviceDisconnected
	};

	enum class InputModifier : uint16_t
	{
		Shift = 1u << 0,
		Control = 1u << 1,
		Alt = 1u << 2,
		Super = 1u << 3,
		CapsLock = 1u << 4,
		NumLock = 1u << 5
	};

	struct InputEvent
	{
		InputEventKind Kind = InputEventKind::Pressed;
		InputControl Control;
		InputWindowID Window = 0;
		uint64_t DeviceInstance = 0;
		float X = 0.0f;
		float Y = 0.0f;
		uint16_t Modifiers = 0;
	};

	enum class InputContext : uint8_t
	{
		Gameplay,
		EditorShortcuts,
		ViewportNavigation,
		TextEntry,
		RuntimeUI
	};

	enum class InputDeviceMask : uint8_t
	{
		None = 0,
		Keyboard = 1u << 0,
		Mouse = 1u << 1,
		Gamepad = 1u << 2,
		All = (1u << 0) | (1u << 1) | (1u << 2)
	};

	constexpr InputDeviceMask operator|(InputDeviceMask Left, InputDeviceMask Right) noexcept
	{
		return static_cast<InputDeviceMask>(static_cast<uint8_t>(Left) | static_cast<uint8_t>(Right));
	}

	[[nodiscard]] constexpr bool HasInputDevice(InputDeviceMask Mask, InputDeviceMask Device) noexcept
	{
		return (static_cast<uint8_t>(Mask) & static_cast<uint8_t>(Device)) != 0;
	}

	struct InputContextActivation
	{
		InputContext Context = InputContext::Gameplay;
		InputDeviceMask Consumes = InputDeviceMask::None;
		InputWindowID OwnerWindow = 0;
		bool Active = false;
	};

	struct InputRoutingState
	{
		std::array<InputContextActivation, 5> Contexts{};
		size_t ContextCount = 0;
		InputWindowID FocusedWindow = 0;
	};

	enum class ActionValueType : uint8_t
	{
		Digital,
		Axis1D,
		Axis2D
	};

	enum class ActionBindingKind : uint8_t
	{
		Button,
		Axis,
		Stick
	};

	struct ActionBinding
	{
		std::string Id;
		ActionBindingKind Kind = ActionBindingKind::Button;
		InputControl Primary;
		InputControl Secondary;
		std::array<InputControl, 4> Modifiers{};
		uint8_t ModifierCount = 0;
		float ScaleX = 1.0f;
		float ScaleY = 0.0f;
		float DeadZone = 0.15f;
	};

	struct GameplayActionDefinition
	{
		std::string Id;
		ActionValueType Type = ActionValueType::Digital;
		InputContext Context = InputContext::Gameplay;
		std::vector<ActionBinding> Bindings;
	};

	struct GameplayActionMap
	{
		uint32_t Version = 1;
		std::vector<GameplayActionDefinition> Actions;
	};

	struct ActionValue
	{
		ActionValueType Type = ActionValueType::Digital;
		bool Held = false;
		bool Pressed = false;
		bool Released = false;
		bool Canceled = false;
		float X = 0.0f;
		float Y = 0.0f;
	};

	struct ActionSnapshotEntry
	{
		std::string Id;
		ActionValue Value;
	};

	struct ActionSnapshot
	{
		uint64_t Frame = 0;
		std::vector<ActionSnapshotEntry> Actions;

		[[nodiscard]] const ActionValue* Find(std::string_view Id) const noexcept;
	};

	struct SimulationActionSnapshot
	{
		uint64_t Frame = 0;
		std::vector<ActionSnapshotEntry> Actions;
	};

	struct GameplayInputError
	{
		std::string Message;
	};

	[[nodiscard]] PULSEFORGE_API std::expected<void, GameplayInputError> ValidateGameplayActionMap(
		const GameplayActionMap& Map);
	[[nodiscard]] PULSEFORGE_API std::expected<GameplayActionMap, GameplayInputError> ParseGameplayActionMap(
		std::string_view Json);
	[[nodiscard]] PULSEFORGE_API std::expected<std::string, GameplayInputError> SerializeGameplayActionMap(
		const GameplayActionMap& Map);

	class PULSEFORGE_API GameplayInputService final
	{
	public:
		static constexpr size_t MaximumQueuedEvents = 4096;
		static constexpr size_t MaximumActions = 256;

		GameplayInputService();
		~GameplayInputService();
		GameplayInputService(const GameplayInputService&) = delete;
		GameplayInputService& operator=(const GameplayInputService&) = delete;
		GameplayInputService(GameplayInputService&&) noexcept;
		GameplayInputService& operator=(GameplayInputService&&) noexcept;

		[[nodiscard]] std::expected<void, GameplayInputError> SetActionMap(GameplayActionMap Map);
		[[nodiscard]] const GameplayActionMap& GetActionMap() const noexcept { return m_Map; }
		[[nodiscard]] bool QueueEvent(const InputEvent& Event) noexcept;
		void PublishFrame(const InputRoutingState& Routing) noexcept;
		void Reset() noexcept;

		[[nodiscard]] const ActionSnapshot& GetSnapshot() const noexcept { return m_Snapshot; }
		[[nodiscard]] const ActionValue* FindAction(std::string_view Id) const noexcept
		{
			return m_Snapshot.Find(Id);
		}
		// Fixed-step consumers receive pending edges at most once. With zero simulation steps, edges remain pending.
		[[nodiscard]] SimulationActionSnapshot BeginSimulationStep();
		[[nodiscard]] bool DidOverflowEvents() const noexcept { return m_EventOverflowed; }

	private:
		struct Impl;
		std::unique_ptr<Impl> m_Impl;
		GameplayActionMap m_Map;
		std::vector<InputEvent> m_Events;
		ActionSnapshot m_Snapshot;
		SimulationActionSnapshot m_PendingSimulationEdges;
		uint64_t m_Frame = 0;
		bool m_EventOverflowed = false;
		bool m_EventOverflowPending = false;
	};
}
