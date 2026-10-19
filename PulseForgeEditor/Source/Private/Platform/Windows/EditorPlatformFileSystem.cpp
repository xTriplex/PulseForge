#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include "Editor/EditorPlatformFileSystem.h"

#include <cstdlib>
#include <string>

namespace PulseForgeEditor
{
	std::expected<std::optional<std::filesystem::path>, std::string>
		EditorPlatformFileSystem::GetSettingsDirectoryOverride()
	{
		if (const wchar_t* Override = _wgetenv(L"PULSEFORGE_EDITOR_SETTINGS_DIR"); Override && Override[0] != L'\0')
			return std::optional<std::filesystem::path>(std::filesystem::path(Override));
		return std::optional<std::filesystem::path>{};
	}

	std::expected<void, std::string> EditorPlatformFileSystem::ReplaceFileAtomically(
		const std::filesystem::path& TemporaryPath, const std::filesystem::path& DestinationPath)
	{
		if (!MoveFileExW(TemporaryPath.c_str(), DestinationPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
			return std::unexpected("Could not atomically replace the destination file (Windows error " +
				std::to_string(GetLastError()) + ")");
		return {};
	}
} // namespace PulseForgeEditor
