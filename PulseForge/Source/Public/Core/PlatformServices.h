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
	[[nodiscard]] PathResult GetResourceDirectory();
	[[nodiscard]] PathResult GetUserDataDirectory();
	[[nodiscard]] std::expected<void, PathError> ReplaceFileAtomically(
		const std::filesystem::path& TemporaryPath,
		const std::filesystem::path& DestinationPath);
	void DebugBreak() noexcept;
}
