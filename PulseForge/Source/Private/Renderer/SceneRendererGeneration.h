#pragma once

#include <cstdint>
#include <limits>

namespace PulseForge::Detail
{
	[[nodiscard]] inline uint64_t AdvanceSceneRendererGeneration(uint64_t Generation) noexcept
	{
		return Generation == (std::numeric_limits<uint64_t>::max)() ? 1 : Generation + 1;
	}
} // namespace PulseForge::Detail
