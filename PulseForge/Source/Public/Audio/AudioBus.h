#pragma once

#include "Core/Core.h"

#include <cstdint>

namespace PulseForge
{
	enum class AudioBus : uint8_t
	{
		Music,
		Effects,
		UI
	};

	struct AudioBusMix
	{
		// Gains are linear multipliers. EffectiveGain includes each mute and applies the master only once.
		float MasterGain = 1.0f;
		float BusGain = 1.0f;
		bool MasterMuted = false;
		bool BusMuted = false;
		float EffectiveGain = 1.0f;
	};
}
