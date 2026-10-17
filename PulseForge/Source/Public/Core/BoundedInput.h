#pragma once

#include "Core/Core.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <istream>
#include <string>

namespace PulseForge::Detail
{
	enum class BoundedInputErrorCode : std::uint8_t
	{
		TooLarge,
		ReadFailed
	};

	struct BoundedInputError
	{
		BoundedInputErrorCode Code;
	};

	[[nodiscard]] PULSEFORGE_API std::expected<std::string, BoundedInputError> ReadBoundedInput(
		std::istream& Input, std::size_t MaximumBytes);
} // namespace PulseForge::Detail
