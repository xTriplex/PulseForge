#include "Editor/EditorPlatformFileSystem.h"

#include <cstdlib>
#include <string>
#include <system_error>

namespace PulseForgeEditor
{
	std::expected<std::optional<std::filesystem::path>, std::string> EditorPlatformFileSystem::GetSettingsDirectoryOverride()
	{
		if (const char* Override = std::getenv("PULSEFORGE_EDITOR_SETTINGS_DIR"); Override && Override[0] != '\0')
			return std::optional<std::filesystem::path>(std::filesystem::path(Override));
		return std::optional<std::filesystem::path>{};
	}

	std::expected<void, std::string> EditorPlatformFileSystem::ReplaceFileAtomically(
		const std::filesystem::path& TemporaryPath, const std::filesystem::path& DestinationPath)
	{
		std::error_code Error;
		std::filesystem::rename(TemporaryPath, DestinationPath, Error);
		if (Error) return std::unexpected("Could not atomically replace the destination file: " + Error.message());
		return {};
	}
} // namespace PulseForgeEditor
