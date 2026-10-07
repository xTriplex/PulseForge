#include "Core/PulseForgePCH.h"
#include "Scene/Components/ScriptComponent.h"

namespace PulseForge
{
	std::expected<void, ScriptComponentError> ScriptComponent::Validate() const
	{
		if (ScriptAsset.IsNil())
			return std::unexpected(ScriptComponentError{
				ScriptComponentErrorCode::InvalidAsset,
				"Script component requires a non-nil script asset UUID" });
		return {};
	}
}
