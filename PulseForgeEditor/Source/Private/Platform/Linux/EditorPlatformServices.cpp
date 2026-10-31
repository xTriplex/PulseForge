#include <GLFW/glfw3.h>

#include "Editor/EditorPlatformServices.h"

#include <string>

namespace PulseForgeEditor
{
	std::expected<void, std::string> EditorPlatformServices::OpenFileWithDefaultApplicationNative(
		const std::filesystem::path& AbsoluteFilePath)
	{
		return OpenFileWithDefaultApplicationPosix("xdg-open", AbsoluteFilePath);
	}

	void EditorPlatformServices::ApplyMainWindowAppearance(GLFWwindow* Window) noexcept
	{
		(void)Window;
	}
} // namespace PulseForgeEditor
