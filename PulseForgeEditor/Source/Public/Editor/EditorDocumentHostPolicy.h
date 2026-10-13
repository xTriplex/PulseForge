#pragma once

namespace PulseForgeEditor
{
	[[nodiscard]] constexpr bool ShouldProcessNativeDocumentHostCloseRequests(
		bool ApplicationShutdownRequested) noexcept
	{
		return !ApplicationShutdownRequested;
	}
}
