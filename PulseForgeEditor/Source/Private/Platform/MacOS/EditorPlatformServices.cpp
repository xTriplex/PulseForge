#include "Editor/EditorPlatformServices.h"

#include <string>

namespace PulseForgeEditor
{
	std::expected<void, std::string> EditorPlatformServices::OpenFileWithDefaultApplicationNative(
		const std::filesystem::path& AbsoluteFilePath)
	{
		return OpenFileWithDefaultApplicationPosix("open", AbsoluteFilePath);
	}

	void EditorPlatformServices::ApplyMainWindowAppearance(GLFWwindow* Window) noexcept
	{
		// Keep the native title bar appearance controlled by macOS and the user's system preference.
		(void)Window;
	}
} // namespace PulseForgeEditor
