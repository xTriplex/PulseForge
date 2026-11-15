#pragma once

#include "Assets/AssetID.h"

#include <expected>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace PulseForge
{
	inline constexpr float MaximumBlueprintFloatPropertyMagnitude = 1'000'000'000.0f;

	struct BlueprintComponent final
	{
		AssetID BlueprintAsset;
		bool Enabled = true;
		// Authored initial scalar values keyed by the Blueprint's stable property identifiers.
		std::map<std::string, float, std::less<>> FloatProperties;

		[[nodiscard]] std::expected<void, std::string> Validate() const;
		[[nodiscard]] std::optional<float> FindFloatProperty(std::string_view Identifier) const noexcept;
		[[nodiscard]] std::expected<void, std::string> SetFloatProperty(std::string_view Identifier, float Value);
		[[nodiscard]] std::expected<void, std::string> RemoveFloatProperty(std::string_view Identifier);
	};
}
