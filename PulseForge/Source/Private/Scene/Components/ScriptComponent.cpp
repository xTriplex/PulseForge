#include "Scene/Components/ScriptComponent.h"

#include <cmath>

namespace PulseForge
{
	bool IsValidScriptPropertyIdentifier(std::string_view Identifier) noexcept
	{
		if (Identifier.empty() || Identifier.size() > MaximumScriptPropertyIdentifierLength) return false;
		const auto IsLetter = [](char Character)
		{ return (Character >= 'a' && Character <= 'z') || (Character >= 'A' && Character <= 'Z'); };
		if (!IsLetter(Identifier.front()) && Identifier.front() != '_') return false;
		for (const char Character : Identifier)
		{
			if (!IsLetter(Character) && !(Character >= '0' && Character <= '9') && Character != '_' && Character != '.' && Character != '-')
				return false;
		}
		return true;
	}

	std::optional<float> ScriptComponent::FindFloatProperty(std::string_view Identifier) const noexcept
	{
		const auto Property = FloatProperties.find(Identifier);
		if (Property == FloatProperties.end()) return std::nullopt;
		return Property->second;
	}

	std::expected<void, ScriptComponentError> ScriptComponent::Validate() const
	{
		if (ScriptAsset.IsNil())
			return std::unexpected(ScriptComponentError{
				ScriptComponentErrorCode::InvalidAsset,
				"Script component requires a non-nil script asset UUID" });
		if (FloatProperties.size() > MaximumScriptFloatPropertyCount)
			return std::unexpected(ScriptComponentError{
				ScriptComponentErrorCode::InvalidPropertyBag, "Script component exceeds the 64 authored float property limit" });
		for (const auto& [Identifier, Value] : FloatProperties)
		{
			if (!IsValidScriptPropertyIdentifier(Identifier))
				return std::unexpected(ScriptComponentError{
					ScriptComponentErrorCode::InvalidPropertyBag, "Script float property IDs must match [A-Za-z_][A-Za-z0-9_.-]{0,63}" });
			if (!std::isfinite(Value) || std::abs(Value) > MaximumScriptFloatPropertyMagnitude)
				return std::unexpected(ScriptComponentError{ ScriptComponentErrorCode::InvalidPropertyBag,
					"Script float property values must be finite and have magnitude at most 1000000000" });
		}
		return {};
	}
}
