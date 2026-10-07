#pragma once

#include "Assets/AssetID.h"
#include "Core/Core.h"

#include <cstdint>
#include <expected>
#include <string>

namespace PulseForge
{
	enum class ScriptComponentErrorCode : uint8_t
	{
		InvalidAsset
	};

	struct ScriptComponentError
	{
		ScriptComponentErrorCode Code;
		std::string Message;
	};

	struct ScriptComponent
	{
		AssetID ScriptAsset;
		bool Enabled = true;

		[[nodiscard]] PULSEFORGE_API std::expected<void, ScriptComponentError> Validate() const;
	};
}
