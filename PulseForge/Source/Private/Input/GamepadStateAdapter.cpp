#include "Input/GamepadStateAdapter.h"

#include <algorithm>
#include <cmath>

namespace PulseForge
{
	namespace
	{
		[[nodiscard]] uint64_t MakeDeviceInstance(uint32_t Slot, uint64_t Generation) noexcept
		{
			return (Generation << 8) | (static_cast<uint64_t>(Slot) + 1);
		}

		[[nodiscard]] float NormalizeAxis(float Value, size_t Axis) noexcept
		{
			if (!std::isfinite(Value)) return 0.0f;
			Value = std::clamp(Value, -1.0f, 1.0f);
			if (Axis >= static_cast<size_t>(GamepadAxis::LeftTrigger)) Value = (Value + 1.0f) * 0.5f;
			return Value;
		}

		void AppendAxisEvent(GamepadStateEvents& Events, uint32_t Slot, uint64_t Generation, size_t Axis, float Value) noexcept
		{
			Events.Values[Events.Count++] = { .Kind = InputEventKind::Value,
				.Control = { InputDevice::GamepadAxis, static_cast<uint16_t>(Axis) },
				.DeviceInstance = MakeDeviceInstance(Slot, Generation), .X = Value };
		}

		void AppendButtonEvent(GamepadStateEvents& Events, uint32_t Slot, uint64_t Generation, size_t Button) noexcept
		{
			Events.Values[Events.Count++] = { .Kind = InputEventKind::Pressed,
				.Control = { InputDevice::GamepadButton, static_cast<uint16_t>(Button) },
				.DeviceInstance = MakeDeviceInstance(Slot, Generation) };
		}
	}

	GamepadStateEvents GamepadStateAdapter::Update(
		uint32_t Slot,
		bool Present,
		bool Mapped,
		const GamepadStateSample* Sample) noexcept
	{
		GamepadStateEvents Events;
		if (Slot >= m_Devices.size()) return Events;

		DeviceState& Device = m_Devices[Slot];
		if (!Present)
		{
			if (Device.Published)
				Events.Values[Events.Count++] = { .Kind = InputEventKind::DeviceDisconnected,
					.DeviceInstance = MakeDeviceInstance(Slot, Device.Generation) };
			Device.Present = false;
			Device.Mapped = false;
			Device.Published = false;
			Device.Initialized = false;
			Device.LastSample = {};
			return Events;
		}

		if (!Device.Present || Device.Mapped != Mapped)
		{
			if (Device.Published)
				Events.Values[Events.Count++] = { .Kind = InputEventKind::DeviceDisconnected,
					.DeviceInstance = MakeDeviceInstance(Slot, Device.Generation) };
			++Device.Generation;
			if (Device.Generation == 0) ++Device.Generation;
			Device.Present = true;
			Device.Mapped = Mapped;
			Device.Published = false;
			Device.Initialized = false;
			Device.LastSample = {};
		}

		if (!Mapped || !Sample) return Events;

		GamepadStateSample Normalized = *Sample;
		for (size_t Axis = 0; Axis < Normalized.Axes.size(); ++Axis)
			Normalized.Axes[Axis] = NormalizeAxis(Normalized.Axes[Axis], Axis);

		const uint64_t Instance = MakeDeviceInstance(Slot, Device.Generation);
		if (!Device.Initialized)
		{
			Events.Values[Events.Count++] = { .Kind = InputEventKind::DeviceConnected, .DeviceInstance = Instance };
			for (size_t Axis = 0; Axis < Normalized.Axes.size(); ++Axis)
				AppendAxisEvent(Events, Slot, Device.Generation, Axis, Normalized.Axes[Axis]);
			for (size_t Button = 0; Button < Normalized.Buttons.size(); ++Button)
				if (Normalized.Buttons[Button]) AppendButtonEvent(Events, Slot, Device.Generation, Button);
			Device.LastSample = Normalized;
			Device.Initialized = true;
			Device.Published = true;
			return Events;
		}

		for (size_t Button = 0; Button < Normalized.Buttons.size(); ++Button)
		{
			if (Normalized.Buttons[Button] == Device.LastSample.Buttons[Button]) continue;
			if (Normalized.Buttons[Button]) AppendButtonEvent(Events, Slot, Device.Generation, Button);
			else Events.Values[Events.Count++] = { .Kind = InputEventKind::Released,
				.Control = { InputDevice::GamepadButton, static_cast<uint16_t>(Button) }, .DeviceInstance = Instance };
		}
		for (size_t Axis = 0; Axis < Normalized.Axes.size(); ++Axis)
		{
			if (Normalized.Axes[Axis] == Device.LastSample.Axes[Axis]) continue;
			AppendAxisEvent(Events, Slot, Device.Generation, Axis, Normalized.Axes[Axis]);
		}
		Device.LastSample = Normalized;
		return Events;
	}
}
