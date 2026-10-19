#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <string>

namespace PulseForgeEditor
{
	// Internal editor file-system operations. This contract has no window-system dependency.
	class EditorPlatformFileSystem final
	{
	public:
		[[nodiscard]] static std::expected<std::optional<std::filesystem::path>, std::string>
			GetSettingsDirectoryOverride();

		[[nodiscard]] static std::expected<void, std::string> ReplaceFileAtomically(
			const std::filesystem::path& TemporaryPath,
			const std::filesystem::path& DestinationPath);
	};
} // namespace PulseForgeEditor
