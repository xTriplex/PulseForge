#pragma once

#include "Input/GameplayInput.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace PulseForge
{
	struct GamepadStateSample
	{
		static constexpr size_t ButtonCount = 15;
		static constexpr size_t AxisCount = 6;

		std::array<bool, ButtonCount> Buttons{};
		// GLFW's raw axis range is [-1, 1]. Triggers are normalized by the adapter to [0, 1].
		std::array<float, AxisCount> Axes{};
	};

	struct GamepadStateEvents
	{
		static constexpr size_t MaximumEvents = GamepadStateSample::ButtonCount + GamepadStateSample::AxisCount + 2;
		std::array<InputEvent, MaximumEvents> Values{};
		size_t Count = 0;

		[[nodiscard]] std::span<const InputEvent> View() const noexcept { return { Values.data(), Count }; }
	};

	// Converts successful complete device samples into one initial baseline, then change-only events.
	class PULSEFORGE_API GamepadStateAdapter final
	{
	public:
		static constexpr size_t MaximumSlots = 16;

		[[nodiscard]] GamepadStateEvents Update(
			uint32_t Slot,
			bool Present,
			bool Mapped,
			const GamepadStateSample* Sample) noexcept;

	private:
		struct DeviceState
		{
			uint64_t Generation = 0;
			bool Present = false;
			bool Mapped = false;
			bool Published = false;
			bool Initialized = false;
			GamepadStateSample LastSample;
		};

		std::array<DeviceState, MaximumSlots> m_Devices{};
	};
}
