#pragma once

#include <expected>
#include <filesystem>
#include <string>

namespace PulseForge::Platform
{
	struct PathError
	{
		std::string Message;
	};

	using PathResult = std::expected<std::filesystem::path, PathError>;

	[[nodiscard]] PathResult GetExecutablePath();
	[[nodiscard]] PathResult GetUserDataDirectory();
	void DebugBreak() noexcept;
}
