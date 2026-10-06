#pragma once

#include "Assets/AssetID.h"
#include "Core/Core.h"

#include <cstdint>
#include <expected>
#include <string>

namespace PulseForge
{
	enum class AudioSourceErrorCode : uint8_t
	{
		InvalidAsset,
		InvalidVolume
	};

	struct AudioSourceError
	{
		AudioSourceErrorCode Code;
		std::string Message;
	};

	struct AudioSourceComponent
	{
		AssetID AudioAsset;
		float Volume = 1.0f;
		bool Looping = false;
		bool PlayOnStart = true;
		bool Spatialized = true;

		[[nodiscard]] PULSEFORGE_API std::expected<void, AudioSourceError> Validate() const;
	};
}
