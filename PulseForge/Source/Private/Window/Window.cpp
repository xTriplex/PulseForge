#include "pfpch.h"
#include "Window/Window.h"

#ifdef PF_PLATFORM_WINDOWS
    #include "Window/Platform/Windows/WindowsWindow.h"
#endif

namespace PulseForge
{
    Window* Window::Create(const WindowProps& Props)
    {
        #ifdef PF_PLATFORM_WINDOWS
            return new WindowsWindow(Props);
        #else
            PF_CORE_ASSERT(false, "Unknown platform!");
            return nullptr;
        #endif
    }
}