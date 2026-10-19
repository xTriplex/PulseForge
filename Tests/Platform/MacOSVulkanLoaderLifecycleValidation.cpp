#include "Core/Log.h"
#include "Window/Platform/GLFW/GLFWWindow.h"

#include <GLFW/glfw3.h>

#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <iostream>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace
{
	class ScopedTemporaryDirectory
	{
	public:
		ScopedTemporaryDirectory()
		{
			const auto Timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
			const std::filesystem::path Root = std::filesystem::temp_directory_path();
			for (std::uint32_t Attempt = 0; Attempt < 16; ++Attempt)
			{
				const std::filesystem::path Candidate = Root /
					("PulseForgeVulkanLoader-" + std::to_string(::getpid()) + "-" + std::to_string(Timestamp) + "-" +
						std::to_string(Attempt));
				std::error_code Error;
				if (std::filesystem::create_directory(Candidate, Error))
				{
					m_Path = Candidate;
					return;
				}
				if (Error && Error != std::errc::file_exists)
					throw std::runtime_error("Could not create the Vulkan loader test directory: " + Error.message());
			}
			throw std::runtime_error("Could not create a unique Vulkan loader test directory");
		}

		~ScopedTemporaryDirectory()
		{
			std::error_code Error;
			std::filesystem::remove_all(m_Path, Error);
		}

		[[nodiscard]] const std::filesystem::path& Path() const noexcept { return m_Path; }

	private:
		std::filesystem::path m_Path;
	};

	struct ScopedTestLibrary
	{
		explicit ScopedTestLibrary(void* Value) noexcept : Handle(Value) {}
		~ScopedTestLibrary() { if (Handle) dlclose(Handle); }
		ScopedTestLibrary(const ScopedTestLibrary&) = delete;
		ScopedTestLibrary& operator=(const ScopedTestLibrary&) = delete;
		void* Handle = nullptr;
	};

	void PrintLoaderEnvironment()
	{
		for (const char* Name : {"VULKAN_SDK", "VK_DRIVER_FILES", "VK_ICD_FILENAMES", "VK_LAYER_PATH"})
		{
			const char* Value = std::getenv(Name);
			std::cout << "MACOS_VULKAN_LOADER_ENV " << Name << '=' << (Value ? Value : "<unset>") << '\n';
		}
		for (char** Entry = environ; Entry && *Entry; ++Entry)
		{
			const std::string_view Variable(*Entry);
			const size_t Separator = Variable.find('=');
			const std::string_view Name = Variable.substr(0, Separator);
			if (Name.starts_with("DYLD_"))
				std::cout << "MACOS_VULKAN_LOADER_ENV " << Variable << '\n';
		}
	}

	bool ReportAbsoluteDlopenSearch(const std::filesystem::path& EmptySdk)
	{
		PrintLoaderEnvironment();
		for (const char* Filename : {"libvulkan.1.dylib", "libvulkan.dylib"})
		{
			const std::filesystem::path Requested = EmptySdk / "lib" / Filename;
			(void)dlerror();
			ScopedTestLibrary Library(dlopen(Requested.c_str(), RTLD_NOW | RTLD_LOCAL));
			if (!Library.Handle)
			{
				const char* LoaderError = dlerror();
				const std::string ErrorCopy = LoaderError ? LoaderError : "dlopen failed without a diagnostic";
				std::cout << "MACOS_DYLD_ABSOLUTE_DLOPEN_PROBE requested=" << Requested.string()
					<< "; load=failed; dlerror=" << ErrorCopy << '\n';
				continue;
			}

			(void)dlerror();
			void* Symbol = dlsym(Library.Handle, "vkGetInstanceProcAddr");
			const char* SymbolError = dlerror();
			const std::string SymbolErrorCopy = SymbolError ? SymbolError : "<none>";
			Dl_info Image{};
			const bool HasImage = Symbol && dladdr(Symbol, &Image) && Image.dli_fname;
			const std::string LoadedImage = HasImage ? Image.dli_fname : "<unavailable>";
			std::error_code RequestedError;
			std::error_code LoadedError;
			const std::filesystem::path CanonicalRequested = std::filesystem::weakly_canonical(Requested, RequestedError);
			const std::filesystem::path CanonicalLoaded = HasImage
				? std::filesystem::weakly_canonical(LoadedImage, LoadedError)
				: std::filesystem::path{};
			const bool SameImage = HasImage && !RequestedError && !LoadedError && CanonicalRequested == CanonicalLoaded;
			const char* SdkValue = std::getenv("VULKAN_SDK");
			std::error_code SdkPathError;
			const std::filesystem::path CanonicalSdkImage = SdkValue && SdkValue[0] != '\0'
				? std::filesystem::weakly_canonical(std::filesystem::u8path(SdkValue) / "lib" / Filename, SdkPathError)
				: std::filesystem::path{};
			const bool SdkImage = HasImage && SdkValue && SdkValue[0] != '\0' && !SdkPathError && !LoadedError &&
				CanonicalSdkImage == CanonicalLoaded;
			std::cout << "MACOS_DYLD_ABSOLUTE_DLOPEN_PROBE requested=" << Requested.string()
				<< "; load=success; symbol=" << (Symbol && !SymbolError ? "success" : "failed")
				<< "; dlerror=" << SymbolErrorCopy << "; image=" << LoadedImage
				<< "; same_image=" << (SameImage ? "true" : "false")
				<< "; sdk_image=" << (SdkImage ? "true" : "false") << '\n';
			return HasImage && !SameImage;
		}
		return false;
	}

	int RunExpectedEmptySdkFailure(const std::filesystem::path& EmptySdk)
	{
		for (char** Entry = environ; Entry && *Entry; ++Entry)
		{
			const std::string_view Variable(*Entry);
			const size_t Separator = Variable.find('=');
			const std::string_view Name = Variable.substr(0, Separator);
			if (Name.starts_with("DYLD_") || Name.starts_with("VK_"))
			{
				std::cerr << "Sanitized loader-failure child inherited an unexpected override: " << Name << '\n';
				return 1;
			}
		}
		if (setenv("VULKAN_SDK", EmptySdk.c_str(), 1) != 0)
		{
			std::cerr << "Could not set the isolated loader-failure VULKAN_SDK" << '\n';
			return 1;
		}
		std::cout << "MACOS_VULKAN_LOADER_SANITIZED_ENV=PASS; DYLD_* and VK_* overrides absent before window startup\n";
		if (!std::filesystem::is_directory(EmptySdk / "lib") ||
			std::filesystem::exists(EmptySdk / "lib/libvulkan.1.dylib") ||
			std::filesystem::exists(EmptySdk / "lib/libvulkan.dylib"))
		{
			std::cerr << "The isolated SDK directory is not empty at the loader candidates" << '\n';
			return 1;
		}

		try
		{
			PulseForge::GLFWWindow Window(PulseForge::WindowProps(
				"PulseForge Vulkan loader acquisition failure", 320, 240, PulseForge::EWindowStyle::Default,
				PulseForge::EWindowClientAPI::Vulkan));
			std::cerr << "Vulkan startup unexpectedly accepted the empty VULKAN_SDK directory" << '\n';
			return 1;
		}
		catch (const std::exception& Error)
		{
			if (std::string_view(Error.what()).find("VULKAN_SDK is set but no loader was loaded from that SDK") ==
				std::string_view::npos)
			{
				std::cerr << "Vulkan startup failed for an unrelated reason: " << Error.what() << '\n';
				return 1;
			}
		}
		if (PulseForge::GLFWWindow::GetVulkanLoaderProcAddress())
		{
			std::cerr << "Failed loader acquisition left a function pointer installed" << '\n';
			return 1;
		}
		std::cout << "MACOS_VULKAN_LOADER_EMPTY_SDK_FAILURE=PASS; reason=selected SDK contained no usable loader\n";
		return 0;
	}

	bool RunSanitizedFailureChild(const char* ExecutablePath, const std::filesystem::path& EmptySdk)
	{
		std::vector<std::string> ChildEnvironment;
		for (char** Entry = environ; Entry && *Entry; ++Entry)
		{
			const std::string_view Variable(*Entry);
			const size_t Separator = Variable.find('=');
			const std::string_view Name = Variable.substr(0, Separator);
			if (Name.starts_with("DYLD_") || Name.starts_with("VK_") || Name == "VULKAN_SDK") continue;
			ChildEnvironment.emplace_back(Variable);
		}
		ChildEnvironment.emplace_back("VULKAN_SDK=" + EmptySdk.string());
		std::vector<char*> EnvironmentPointers;
		EnvironmentPointers.reserve(ChildEnvironment.size() + 1);
		for (std::string& Entry : ChildEnvironment) EnvironmentPointers.push_back(Entry.data());
		EnvironmentPointers.push_back(nullptr);

		std::string Executable(ExecutablePath);
		std::string Mode("--invalid-sdk-child");
		std::string EmptySdkArgument = EmptySdk.string();
		char* Arguments[] = {Executable.data(), Mode.data(), EmptySdkArgument.data(), nullptr};
		pid_t Child = 0;
		const int SpawnError = posix_spawn(&Child, Executable.c_str(), nullptr, nullptr, Arguments, EnvironmentPointers.data());
		if (SpawnError != 0)
		{
			std::cerr << "Could not launch sanitized loader-failure child: " << std::strerror(SpawnError) << '\n';
			return false;
		}

		const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(25);
		int Status = 0;
		for (;;)
		{
			const pid_t Result = waitpid(Child, &Status, WNOHANG);
			if (Result == Child)
			{
				if (WIFEXITED(Status) && WEXITSTATUS(Status) == 0) return true;
				std::cerr << "Sanitized loader-failure child returned status " << Status << '\n';
				return false;
			}
			if (Result < 0 && errno != EINTR)
			{
				std::cerr << "Could not wait for sanitized loader-failure child: " << std::strerror(errno) << '\n';
				if (errno != ECHILD) (void)kill(Child, SIGKILL);
				while (waitpid(Child, &Status, 0) < 0 && errno == EINTR) {}
				return false;
			}
			if (std::chrono::steady_clock::now() >= Deadline)
			{
				std::cerr << "Sanitized loader-failure child exceeded its 25 second bound" << '\n';
				(void)kill(Child, SIGKILL);
				while (waitpid(Child, &Status, 0) < 0 && errno == EINTR) {}
				return false;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
	}

	bool CreateAndCloseVulkanWindow(const char* Title)
	{
		try
		{
			PulseForge::GLFWWindow Window(PulseForge::WindowProps(
				Title, 320, 240, PulseForge::EWindowStyle::Default, PulseForge::EWindowClientAPI::Vulkan));
			if (!PulseForge::GLFWWindow::GetVulkanLoaderProcAddress())
				throw std::runtime_error("GLFW lost its Vulkan loader while the window runtime was active");
			if (glfwVulkanSupported() != GLFW_TRUE)
				throw std::runtime_error("GLFW could not query Vulkan through the injected loader");
		}
		catch (const std::exception& Error)
		{
			std::cerr << "Vulkan loader lifecycle window failed: " << Error.what() << '\n';
			return false;
		}
		if (PulseForge::GLFWWindow::GetVulkanLoaderProcAddress())
		{
			std::cerr << "GLFW retained a Vulkan loader after its last window terminated" << '\n';
			return false;
		}
		return true;
	}
}

int main(int ArgumentCount, char** Arguments)
{
	PulseForge::Log::Init();
	if (ArgumentCount == 3 && std::string_view(Arguments[1]) == "--invalid-sdk-child")
		return RunExpectedEmptySdkFailure(Arguments[2]);
	if (ArgumentCount != 1)
	{
		std::cerr << "Unexpected macOS Vulkan loader lifecycle arguments" << '\n';
		return 1;
	}

	const char* ExistingSdkValue = std::getenv("VULKAN_SDK");
	const std::string ExistingSdk = ExistingSdkValue ? ExistingSdkValue : "";
	const bool HadSdkValue = ExistingSdkValue != nullptr;
	ScopedTemporaryDirectory EmptySdk;
	std::filesystem::create_directory(EmptySdk.Path() / "lib");
	const bool DyldRedirectedAbsolutePath = ReportAbsoluteDlopenSearch(EmptySdk.Path());
	std::error_code ExecutableError;
	const std::filesystem::path ExecutablePath = std::filesystem::canonical(Arguments[0], ExecutableError);
	if (ExecutableError)
	{
		std::cerr << "Could not resolve lifecycle test executable path: " << ExecutableError.message() << '\n';
		return 1;
	}
	if (!RunSanitizedFailureChild(ExecutablePath.string().c_str(), EmptySdk.Path())) return 1;

	if (setenv("VULKAN_SDK", EmptySdk.Path().c_str(), 1) != 0)
	{
		std::cerr << "Could not set the in-process loader-failure VULKAN_SDK" << '\n';
		return 1;
	}
	try
	{
		PulseForge::GLFWWindow InvalidLoaderWindow(PulseForge::WindowProps(
			"PulseForge Vulkan loader acquisition failure", 320, 240, PulseForge::EWindowStyle::Default,
			PulseForge::EWindowClientAPI::Vulkan));
		std::cerr << "Vulkan startup unexpectedly accepted the empty VULKAN_SDK directory" << '\n';
		return 1;
	}
	catch (const std::exception& Error)
	{
		const std::string_view Message(Error.what());
		if (Message.find("VULKAN_SDK is set but no loader was loaded from that SDK") == std::string_view::npos)
		{
			std::cerr << "Vulkan startup failed for an unrelated reason: " << Error.what() << '\n';
			return 1;
		}
		if (DyldRedirectedAbsolutePath &&
			Message.find("DYLD_LIBRARY_PATH may have selected a different Vulkan loader") == std::string_view::npos)
		{
			std::cerr << "The loader path probe observed dyld redirection, but SDK selection did not reject it: "
				<< Error.what() << '\n';
			return 1;
		}
		std::cout << "MACOS_VULKAN_LOADER_FAILURE_REASON=" << Error.what() << '\n';
	}
	if (PulseForge::GLFWWindow::GetVulkanLoaderProcAddress())
	{
		std::cerr << "Failed loader acquisition left a function pointer installed" << '\n';
		return 1;
	}

	if (HadSdkValue)
		setenv("VULKAN_SDK", ExistingSdk.c_str(), 1);
	else
		unsetenv("VULKAN_SDK");

	if (!CreateAndCloseVulkanWindow("PulseForge Vulkan loader startup")) return 1;
	if (!CreateAndCloseVulkanWindow("PulseForge Vulkan loader repeated initialization")) return 1;
	{
		PulseForge::GLFWWindow OpenGLWindow(PulseForge::WindowProps(
			"PulseForge OpenGL runtime", 320, 240, PulseForge::EWindowStyle::Default, PulseForge::EWindowClientAPI::OpenGL));
		try
		{
			PulseForge::GLFWWindow MismatchedVulkanWindow(PulseForge::WindowProps(
				"PulseForge Vulkan cannot join OpenGL runtime", 320, 240, PulseForge::EWindowStyle::Default,
				PulseForge::EWindowClientAPI::Vulkan));
			std::cerr << "Vulkan unexpectedly joined a GLFW runtime initialized without its loader" << '\n';
			return 1;
		}
		catch (const std::exception& Error)
		{
			if (std::string_view(Error.what()).find("cannot join a GLFW runtime initialized without its Vulkan loader") ==
				std::string_view::npos)
			{
				std::cerr << "The Vulkan/OpenGL runtime mismatch failed for an unrelated reason: " << Error.what() << '\n';
				return 1;
			}
			if (PulseForge::GLFWWindow::GetVulkanLoaderProcAddress())
			{
				std::cerr << "Rejected Vulkan window left a loader installed into the OpenGL runtime" << '\n';
				return 1;
			}
		}
	}
	if (PulseForge::GLFWWindow::GetVulkanLoaderProcAddress())
	{
		std::cerr << "OpenGL runtime teardown retained a Vulkan loader" << '\n';
		return 1;
	}
	if (setenv("VULKAN_SDK", "/path/that/does/not/exist", 1) != 0) return 1;
	try
	{
		PulseForge::GLFWWindow OpenGLWindow(PulseForge::WindowProps(
			"PulseForge OpenGL does not require a Vulkan SDK", 320, 240, PulseForge::EWindowStyle::Default,
			PulseForge::EWindowClientAPI::OpenGL));
		if (PulseForge::GLFWWindow::GetVulkanLoaderProcAddress())
			throw std::runtime_error("OpenGL startup unexpectedly acquired a Vulkan loader");
	}
	catch (const std::exception& Error)
	{
		std::cerr << "OpenGL startup unexpectedly required Vulkan: " << Error.what() << '\n';
		return 1;
	}
	if (HadSdkValue)
		setenv("VULKAN_SDK", ExistingSdk.c_str(), 1);
	else
		unsetenv("VULKAN_SDK");

	std::cout << "MACOS_VULKAN_LOADER_LIFECYCLE=PASS; sanitized failure, explicit SDK selection, startup, API mismatch, "
		"termination and repeated initialization\n";
	return 0;
}
