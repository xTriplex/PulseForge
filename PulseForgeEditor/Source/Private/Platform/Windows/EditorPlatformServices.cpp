#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <string>

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include "Core/Log.h"
#include "Editor/EditorPlatformServices.h"

namespace PulseForgeEditor
{
	std::expected<void, std::string> EditorPlatformServices::OpenFileWithDefaultApplicationNative(
		const std::filesystem::path& AbsoluteFilePath)
	{
		const HINSTANCE Result = ShellExecuteW(nullptr, L"open", AbsoluteFilePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
		const INT_PTR ResultCode = reinterpret_cast<INT_PTR>(Result);
		if (ResultCode <= 32)
			return std::unexpected("Windows could not open the script with its associated application (ShellExecute code " +
				std::to_string(ResultCode) + ")");
		return {};
	}

	void EditorPlatformServices::ApplyMainWindowAppearance(GLFWwindow* Window) noexcept
	{
		const HWND NativeWindow = Window ? glfwGetWin32Window(Window) : nullptr;
		if (!NativeWindow) return;

		const BOOL UseDarkMode = TRUE;
		const HRESULT Result = DwmSetWindowAttribute(NativeWindow, DWMWA_USE_IMMERSIVE_DARK_MODE, &UseDarkMode, sizeof(UseDarkMode));
		if (FAILED(Result)) PF_INFO("Dark native caption styling is unavailable; using the system caption style");
	}
} // namespace PulseForgeEditor
