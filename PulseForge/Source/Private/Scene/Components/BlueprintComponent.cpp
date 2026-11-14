#include "Scene/Components/BlueprintComponent.h"

#include <cmath>
#include <limits>

namespace PulseForge
{
	namespace
	{
		constexpr size_t MaximumBlueprintPropertyCount = 64;
		constexpr size_t MaximumBlueprintPropertyIdentifierLength = 64;
		constexpr float MaximumBlueprintPropertyMagnitude = 1'000'000'000.0f;

		bool IsValidIdentifier(std::string_view Identifier) noexcept
		{
			if (Identifier.empty() || Identifier.size() > MaximumBlueprintPropertyIdentifierLength) return false;
			for (const unsigned char Character : Identifier)
				if (!(Character >= 'a' && Character <= 'z') && !(Character >= 'A' && Character <= 'Z') &&
					!(Character >= '0' && Character <= '9') && Character != '_' && Character != '.' && Character != '-')
					return false;
			return true;
		}
	}

	std::expected<void, std::string> BlueprintComponent::Validate() const
	{
		if (BlueprintAsset.IsNil()) return std::unexpected("Blueprint component requires a non-nil asset UUID");
		if (FloatProperties.size() > MaximumBlueprintPropertyCount)
			return std::unexpected("Blueprint component exceeds the scalar property limit");
		for (const auto& [Identifier, Value] : FloatProperties)
		{
			if (!IsValidIdentifier(Identifier))
				return std::unexpected("Blueprint scalar property identifier is invalid");
			if (!std::isfinite(Value) || std::abs(Value) > MaximumBlueprintPropertyMagnitude)
				return std::unexpected("Blueprint scalar property value must be finite and bounded");
		}
		return {};
	}

	std::optional<float> BlueprintComponent::FindFloatProperty(std::string_view Identifier) const noexcept
	{
		const auto Found = FloatProperties.find(Identifier);
		return Found == FloatProperties.end() ? std::nullopt : std::optional<float>{ Found->second };
	}

	std::expected<void, std::string> BlueprintComponent::SetFloatProperty(std::string_view Identifier, float Value)
	{
		if (!IsValidIdentifier(Identifier)) return std::unexpected("Blueprint scalar property identifier is invalid");
		if (!std::isfinite(Value) || std::abs(Value) > MaximumBlueprintPropertyMagnitude)
			return std::unexpected("Blueprint scalar property value must be finite and bounded");
		if (!FloatProperties.contains(Identifier) && FloatProperties.size() >= MaximumBlueprintPropertyCount)
			return std::unexpected("Blueprint component exceeds the scalar property limit");
		FloatProperties.insert_or_assign(std::string(Identifier), Value);
		return {};
	}
}
