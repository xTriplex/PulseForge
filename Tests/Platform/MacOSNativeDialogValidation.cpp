#include "Editor/EditorPlatformServices.h"

#include <GLFW/glfw3.h>

#include <chrono>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace
{
	std::string PathToUtf8(const std::filesystem::path& Path)
	{
		const std::u8string Encoded = Path.u8string();
		return { reinterpret_cast<const char*>(Encoded.data()), Encoded.size() };
	}

	bool IsScenario(std::string_view Scenario)
	{
		return Scenario == "open-select" || Scenario == "open-cancel" || Scenario == "save-select" || Scenario == "save-cancel";
	}

	int Fail(std::string_view Message)
	{
		std::cerr << "NFD_FIXTURE=FAIL; " << Message << '\n';
		return 1;
	}

	bool WaitForWindowFocus(GLFWwindow* Window)
	{
		using Clock = std::chrono::steady_clock;
		const Clock::time_point Deadline = Clock::now() + std::chrono::seconds(2);
		while (Clock::now() < Deadline)
		{
			glfwPollEvents();
			if (glfwWindowShouldClose(Window)) return false;
			if (glfwGetWindowAttrib(Window, GLFW_FOCUSED) == GLFW_TRUE) return true;
			glfwWaitEventsTimeout(0.05);
		}
		glfwPollEvents();
		return glfwGetWindowAttrib(Window, GLFW_FOCUSED) == GLFW_TRUE;
	}

	class GLFWLifetime final
	{
	public:
		~GLFWLifetime() { glfwTerminate(); }
	};

	class DialogLifetime final
	{
	public:
		~DialogLifetime() { PulseForgeEditor::EditorPlatformServices::ShutdownNativeFileDialogs(); }
	};
} // namespace

int main(int ArgumentCount, char** Arguments)
{
	if (ArgumentCount != 5 || std::string_view(Arguments[1]) != "--scenario" || std::string_view(Arguments[3]) != "--path")
	{
		std::cerr << "Usage: PulseForgeNativeDialogValidation --scenario <open-select|open-cancel|save-select|save-cancel> "
					 "--path <absolute-path>\n";
		return 2;
	}

	const std::string_view Scenario = Arguments[2];
	const std::filesystem::path ExpectedPath = std::filesystem::absolute(std::filesystem::u8path(Arguments[4])).lexically_normal();
	if (!IsScenario(Scenario) || !ExpectedPath.is_absolute() || ExpectedPath.filename().empty())
		return Fail("scenario or expected path is invalid");
	const bool IsOpen = Scenario.starts_with("open-");
	const bool IsSelection = Scenario.ends_with("select");
	std::error_code PathError;
	if (IsOpen && IsSelection && !std::filesystem::is_regular_file(ExpectedPath, PathError))
		return Fail("the expected Open selection must be an existing regular file");
	if (IsOpen && IsSelection && PathError) return Fail("could not inspect the expected Open selection");
	if (!std::filesystem::is_directory(ExpectedPath.parent_path(), PathError))
		return Fail("the expected dialog parent directory must exist");
	if (PathError) return Fail("could not inspect the expected dialog parent directory");
	if (!IsSelection && std::filesystem::exists(ExpectedPath, PathError))
		return Fail("a cancellation scenario's expected path must not exist");
	if (!IsSelection && PathError) return Fail("could not inspect the cancellation path");
	if (Scenario == "save-select" && std::filesystem::exists(ExpectedPath, PathError))
		return Fail("the expected Save selection must not already exist");
	if (Scenario == "save-select" && PathError) return Fail("could not inspect the expected Save path");

	if (glfwInit() != GLFW_TRUE) return Fail("GLFW initialization failed");
	GLFWLifetime GLFWCleanup;
	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
	glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
	GLFWwindow* Window = glfwCreateWindow(720, 420, "PulseForge Native Dialog Validation", nullptr, nullptr);
	if (!Window) return Fail("could not create the native Cocoa parent window");
	struct WindowCleanup final
	{
		GLFWwindow* Window;
		~WindowCleanup()
		{
			if (Window) glfwDestroyWindow(Window);
		}
	} WindowLifetime{ Window };
	glfwShowWindow(Window);
	glfwFocusWindow(Window);
	glfwPollEvents();

	const auto Initialized = PulseForgeEditor::EditorPlatformServices::InitializeNativeFileDialogs();
	if (!Initialized) return Fail("native dialog initialization failed: " + Initialized.error());
	DialogLifetime DialogCleanup;
	const std::filesystem::path InitialDirectory = ExpectedPath.parent_path();
	std::cout << "NFD_DIALOG_READY=" << Scenario << "; expected=" << PathToUtf8(ExpectedPath)
			  << "; initial_directory=" << PathToUtf8(InitialDirectory)
			  << "; expected_filename=" << PathToUtf8(ExpectedPath.filename()) << std::endl;
	std::expected<std::optional<std::filesystem::path>, std::string> Result =
		IsOpen ? PulseForgeEditor::EditorPlatformServices::ShowOpenFileDialog(Window, nullptr, InitialDirectory)
			   : PulseForgeEditor::EditorPlatformServices::ShowSaveFileDialog(
					 Window, nullptr, InitialDirectory, PathToUtf8(ExpectedPath.filename()));
	if (!Result) return Fail("adapter returned an error: " + Result.error());

	glfwPollEvents();
	if (glfwWindowShouldClose(Window) || glfwGetWindowAttrib(Window, GLFW_VISIBLE) != GLFW_TRUE)
		return Fail("parent window was not usable after the native dialog returned");
	if (!WaitForWindowFocus(Window)) return Fail("parent window did not regain focus after the native dialog returned");
	glfwSetWindowTitle(Window, "PulseForge Native Dialog Validation · returned");
	std::cout << "NFD_DIALOG_FOCUS=PASS; scenario=" << Scenario << std::endl;
	std::cout << "NFD_DIALOG_POST_USE=PASS; scenario=" << Scenario << std::endl;

	if (!IsSelection)
	{
		if (*Result) return Fail("cancellation returned a selected path");
		std::cout << "NFD_DIALOG_RESULT=CANCELLED; scenario=" << Scenario << std::endl;
		return 0;
	}
	if (!*Result) return Fail("selection returned cancellation");
	const std::filesystem::path ActualPath = std::filesystem::absolute(**Result).lexically_normal();
	if (ActualPath != ExpectedPath)
		return Fail("selected path mismatch; actual=" + PathToUtf8(ActualPath) + "; expected=" + PathToUtf8(ExpectedPath));
	if (!IsOpen)
	{
		std::ofstream SavedFile(ActualPath, std::ios::binary | std::ios::trunc);
		SavedFile << "PulseForge native dialog smoke Ω\n";
		SavedFile.close();
		if (!SavedFile) return Fail("selected save path could not be written");
	}
	std::cout << "NFD_DIALOG_RESULT=SELECTED; scenario=" << Scenario << "; path=" << PathToUtf8(ActualPath) << std::endl;
	return 0;
}
