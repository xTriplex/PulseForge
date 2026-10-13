#pragma once

#include <vector>

namespace PulseForgeEditor
{
	[[nodiscard]] constexpr bool ShouldProcessNativeDocumentHostCloseRequests(
		bool ApplicationShutdownRequested) noexcept
	{
		return !ApplicationShutdownRequested;
	}

	template<typename Identity, typename IsDirtyPredicate>
	[[nodiscard]] std::vector<Identity> FilterDirtyDocumentIdentities(
		const std::vector<Identity>& Identities,
		IsDirtyPredicate&& IsDirty)
	{
		std::vector<Identity> DirtyIdentities;
		for (const Identity& Identity : Identities)
			if (IsDirty(Identity))
				DirtyIdentities.push_back(Identity);
		return DirtyIdentities;
	}
}
