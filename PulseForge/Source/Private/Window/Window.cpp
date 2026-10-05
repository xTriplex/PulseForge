#include "Core/PulseForgePCH.h"
#include "Window/Window.h"

#ifdef PF_PLATFORM_WINDOWS
    #include "Window/Platform/Windows/WindowsWindow.h"
#endif

namespace PulseForge
{
    std::unique_ptr<Window> Window::Create(const WindowProps& Props)
    {
        #ifdef PF_PLATFORM_WINDOWS
            return std::make_unique<WindowsWindow>(Props);
        #else
            PF_CORE_ASSERT(false, "Unknown platform!");
            return {};
        #endif
    }
}
