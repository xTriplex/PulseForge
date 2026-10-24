#pragma once

#include "Assets/AssetID.h"
#include "Core/Core.h"

#include <cstdint>
#include <cstddef>
#include <expected>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace PulseForge
{
	inline constexpr size_t MaximumScriptFloatPropertyCount = 64;
	inline constexpr size_t MaximumScriptPropertyIdentifierLength = 64;
	inline constexpr float MaximumScriptFloatPropertyMagnitude = 1000000000.0f;

	[[nodiscard]] PULSEFORGE_API bool IsValidScriptPropertyIdentifier(std::string_view Identifier) noexcept;

	enum class ScriptComponentErrorCode : uint8_t
	{
		InvalidAsset,
		InvalidPropertyBag
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
		// Per-entity authored scalar values addressed by stable IDs declared by the script asset.
		std::map<std::string, float, std::less<>> FloatProperties;

		[[nodiscard]] PULSEFORGE_API std::expected<void, ScriptComponentError> Validate() const;
		[[nodiscard]] std::optional<float> FindFloatProperty(std::string_view Identifier) const noexcept;
	};
}
