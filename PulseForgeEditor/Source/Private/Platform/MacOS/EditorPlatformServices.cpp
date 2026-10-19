#include "Editor/EditorPlatformServices.h"

namespace PulseForgeEditor
{
	void EditorPlatformServices::ApplyMainWindowAppearance(GLFWwindow* Window) noexcept
	{
		// Keep the native title bar appearance controlled by macOS and the user's system preference.
		(void)Window;
	}
} // namespace PulseForgeEditor
