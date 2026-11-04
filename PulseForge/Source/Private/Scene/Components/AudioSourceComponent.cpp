#include "Scene/Components/AudioSourceComponent.h"

#include <cmath>

namespace PulseForge
{
	std::expected<void, AudioSourceError> AudioSourceComponent::Validate() const
	{
		if (AudioAsset.IsNil())
		{
			return std::unexpected(AudioSourceError{
				AudioSourceErrorCode::InvalidAsset,
				"Audio source requires a non-nil audio asset UUID" });
		}
		if (!std::isfinite(Volume) || Volume < 0.0f || Volume > 1.0f)
		{
			return std::unexpected(AudioSourceError{
				AudioSourceErrorCode::InvalidVolume,
				"Audio source volume must be finite and between zero and one" });
		}
		if (Bus != AudioBus::Music && Bus != AudioBus::Effects && Bus != AudioBus::UI)
			return std::unexpected(AudioSourceError{ AudioSourceErrorCode::InvalidBus, "Audio source bus is not supported" });
		return {};
	}
}
