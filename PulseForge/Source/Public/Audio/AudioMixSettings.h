#pragma once

#include "Audio/AudioBus.h"
#include "Core/Core.h"

#include <cmath>
#include <optional>

namespace PulseForge
{
	struct AudioMixGains final
	{
		float Master = 1.0f;
		float Music = 1.0f;
		float Effects = 1.0f;
		float UI = 1.0f;
		friend bool operator==(const AudioMixGains&, const AudioMixGains&) = default;

		[[nodiscard]] float Get(AudioBus Bus) const noexcept
		{
			switch (Bus)
			{
				case AudioBus::Music: return Music;
				case AudioBus::Effects: return Effects;
				case AudioBus::UI: return UI;
			}
			return 0.0f;
		}

		[[nodiscard]] bool IsValid() const noexcept
		{
			const auto ValidGain = [](float Gain) { return std::isfinite(Gain) && Gain >= 0.0f && Gain <= 1.0f; };
			return ValidGain(Master) && ValidGain(Music) && ValidGain(Effects) && ValidGain(UI);
		}
	};

	struct AudioUserMixOverrides final
	{
		std::optional<float> Master;
		std::optional<float> Music;
		std::optional<float> Effects;
		std::optional<float> UI;
		bool MasterMuted = false;
		bool MusicMuted = false;
		bool EffectsMuted = false;
		bool UIMuted = false;

		[[nodiscard]] const std::optional<float>& Get(AudioBus Bus) const noexcept
		{
			switch (Bus)
			{
				case AudioBus::Music: return Music;
				case AudioBus::Effects: return Effects;
				case AudioBus::UI: return UI;
			}
			return Effects;
		}
	};

	struct AudioMixState final
	{
		AudioMixGains Gains;
		bool MasterMuted = false;
		bool MusicMuted = false;
		bool EffectsMuted = false;
		bool UIMuted = false;

		[[nodiscard]] bool IsMuted(AudioBus Bus) const noexcept
		{
			if (MasterMuted) return true;
			switch (Bus)
			{
				case AudioBus::Music: return MusicMuted;
				case AudioBus::Effects: return EffectsMuted;
				case AudioBus::UI: return UIMuted;
			}
			return true;
		}
	};
}
