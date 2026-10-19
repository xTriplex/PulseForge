#include "Core/Log.h"
#include "Window/Window.h"

#if defined(PF_PLATFORM_WINDOWS) || defined(PF_PLATFORM_LINUX) || defined(PF_PLATFORM_MACOS)
#include "Window/Platform/GLFW/GLFWWindow.h"
#endif

namespace PulseForge
{
	std::unique_ptr<Window> Window::Create(const WindowProps& Props)
	{
#if defined(PF_PLATFORM_WINDOWS) || defined(PF_PLATFORM_LINUX) || defined(PF_PLATFORM_MACOS)
		return std::make_unique<GLFWWindow>(Props);
#else
		PF_CORE_ASSERT(false, "Unknown platform!");
		return {};
#endif
	}
}
