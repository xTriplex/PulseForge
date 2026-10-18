#pragma once

#include <memory>

namespace spdlog::sinks
{
	class sink;
}

namespace PulseForge::Platform::Detail
{
	// Internal implementation boundary used by Core logging; this is not a consumer-facing logging API.
	[[nodiscard]] std::shared_ptr<spdlog::sinks::sink> CreateLogSink();
}
