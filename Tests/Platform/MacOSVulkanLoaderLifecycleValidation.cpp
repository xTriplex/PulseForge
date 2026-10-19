#include "Core/Log.h"
#include "Window/Platform/GLFW/GLFWWindow.h"

#include <GLFW/glfw3.h>

#include <array>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdio>
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
	struct FGLFWProbeError
	{
		int Code = GLFW_NO_ERROR;
		std::array<char, 1024> Description{};
	};

	FGLFWProbeError s_GLFWProbeError;

	void CaptureGLFWProbeError(int Code, const char* Description) noexcept
	{
		s_GLFWProbeError.Code = Code;
		std::snprintf(s_GLFWProbeError.Description.data(), s_GLFWProbeError.Description.size(), "%s",
			Description ? Description : "");
	}

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

	void ReportAbsoluteDlopenSearch(const std::filesystem::path& EmptySdk)
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
				? std::filesystem::weakly_canonical(std::filesystem::path(SdkValue) / "lib" / Filename, SdkPathError)
				: std::filesystem::path{};
			const bool SdkImage = HasImage && SdkValue && SdkValue[0] != '\0' && !SdkPathError && !LoadedError &&
				CanonicalSdkImage == CanonicalLoaded;
			std::cout << "MACOS_DYLD_ABSOLUTE_DLOPEN_PROBE requested=" << Requested.string()
				<< "; load=success; symbol=" << (Symbol && !SymbolError ? "success" : "failed")
				<< "; dlerror=" << SymbolErrorCopy << "; image=" << LoadedImage
				<< "; same_image=" << (SameImage ? "true" : "false")
				<< "; sdk_image=" << (SdkImage ? "true" : "false") << '\n';
			return;
		}
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

	bool VerifySelectedVulkanSdkImage();

	bool CreateAndCloseVulkanWindow(const char* Title)
	{
		try
		{
			PulseForge::GLFWWindow Window(PulseForge::WindowProps(
				Title, 320, 240, PulseForge::EWindowStyle::Default, PulseForge::EWindowClientAPI::Vulkan));
			if (!PulseForge::GLFWWindow::GetVulkanLoaderProcAddress())
				throw std::runtime_error("GLFW lost its Vulkan loader while the window runtime was active");
			const char* SdkValue = std::getenv("VULKAN_SDK");
			if (SdkValue && SdkValue[0] != '\0' && !VerifySelectedVulkanSdkImage())
				throw std::runtime_error("the active Vulkan loader did not come from the selected VULKAN_SDK");
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

	bool VerifySelectedVulkanSdkImage()
	{
		const char* SdkValue = std::getenv("VULKAN_SDK");
		if (!SdkValue || SdkValue[0] == '\0')
		{
			std::cout << "MACOS_VULKAN_LOADER_VALID_SDK=NOT_APPLICABLE; VULKAN_SDK is unset\n";
			return true;
		}

		const auto ProcAddress = PulseForge::GLFWWindow::GetVulkanLoaderProcAddress();
		Dl_info Image{};
		if (!ProcAddress || !dladdr(reinterpret_cast<const void*>(ProcAddress), &Image) || !Image.dli_fname)
		{
			std::cerr << "Could not identify the loaded image for the selected Vulkan SDK loader" << '\n';
			return false;
		}

		const std::filesystem::path LoadedImagePath(Image.dli_fname);
		std::error_code LoadedError;
		const std::filesystem::path LoadedImage = std::filesystem::weakly_canonical(LoadedImagePath, LoadedError);
		if (LoadedError)
		{
			std::cerr << "Could not canonicalize the loaded Vulkan image '" << LoadedImagePath.string() << "': "
				<< LoadedError.message() << '\n';
			return false;
		}

		for (const char* Filename : {"libvulkan.1.dylib", "libvulkan.dylib"})
		{
			std::error_code CandidateError;
			const std::filesystem::path Candidate = std::filesystem::weakly_canonical(
				std::filesystem::path(SdkValue) / "lib" / Filename, CandidateError);
			if (!CandidateError && Candidate == LoadedImage)
			{
				std::cout << "MACOS_VULKAN_LOADER_VALID_SDK=PASS; image=" << LoadedImage.string() << '\n';
				return true;
			}
		}

		std::cerr << "The explicitly selected VULKAN_SDK did not provide the active loader image: "
			<< LoadedImage.string() << '\n';
		return false;
	}
}

static int RunVulkanLoaderLifecycle(const char* ExecutableArgument)
{
	const char* ExistingSdkValue = std::getenv("VULKAN_SDK");
	const std::string ExistingSdk = ExistingSdkValue ? ExistingSdkValue : "";
	const bool HadSdkValue = ExistingSdkValue != nullptr;
	ScopedTemporaryDirectory EmptySdk;
	std::filesystem::create_directory(EmptySdk.Path() / "lib");
	ReportAbsoluteDlopenSearch(EmptySdk.Path());
	std::error_code ExecutableError;
	const std::filesystem::path ExecutablePath = std::filesystem::canonical(
		std::filesystem::path(ExecutableArgument), ExecutableError);
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

	std::cout << "MACOS_VULKAN_LOADER_LIFECYCLE=PASS; sanitized failure, explicit SDK selection, Vulkan startup, "
		"termination and repeated initialization\n";
	return 0;
}

static int RunVulkanLoaderMismatchPolicy()
{
	const char* ExistingSdkValue = std::getenv("VULKAN_SDK");
	const std::string ExistingSdk = ExistingSdkValue ? ExistingSdkValue : "";
	const bool HadSdkValue = ExistingSdkValue != nullptr;
	if (setenv("VULKAN_SDK", "/path/that/does/not/exist", 1) != 0)
	{
		std::cerr << "Could not select the invalid Vulkan SDK for the runtime mismatch policy check" << '\n';
		return 1;
	}

	{
		PulseForge::GLFWWindow NoClientApiWindow(PulseForge::WindowProps(
			"PulseForge no-client-API runtime policy fixture", 320, 240, PulseForge::EWindowStyle::Default,
			PulseForge::EWindowClientAPI::None));
		if (PulseForge::GLFWWindow::GetVulkanLoaderProcAddress())
		{
			std::cerr << "A no-client-API GLFW runtime unexpectedly acquired a Vulkan loader" << '\n';
			return 1;
		}

		try
		{
			PulseForge::GLFWWindow MismatchedVulkanWindow(PulseForge::WindowProps(
				"PulseForge Vulkan cannot join no-client-API runtime", 320, 240, PulseForge::EWindowStyle::Default,
				PulseForge::EWindowClientAPI::Vulkan));
			std::cerr << "Vulkan unexpectedly joined a GLFW runtime initialized without its Vulkan loader" << '\n';
			return 1;
		}
		catch (const std::exception& Error)
		{
			if (std::string_view(Error.what()).find(
					"cannot join a GLFW runtime initialized without its Vulkan loader") == std::string_view::npos)
			{
				std::cerr << "Vulkan runtime mismatch failed for an unrelated reason: " << Error.what() << '\n';
				return 1;
			}
		}
		if (PulseForge::GLFWWindow::GetVulkanLoaderProcAddress())
		{
			std::cerr << "Rejected Vulkan window left a loader installed in the no-client-API runtime" << '\n';
			return 1;
		}
	}

	if (PulseForge::GLFWWindow::GetVulkanLoaderProcAddress())
	{
		std::cerr << "No-client-API runtime teardown retained a Vulkan loader" << '\n';
		return 1;
	}
	if (HadSdkValue)
		setenv("VULKAN_SDK", ExistingSdk.c_str(), 1);
	else
		unsetenv("VULKAN_SDK");
	if (!CreateAndCloseVulkanWindow("PulseForge Vulkan startup after runtime mismatch rejection")) return 1;

	std::cout << "MACOS_VULKAN_RUNTIME_MISMATCH_POLICY=PASS; GLFW_NO_API policy fixture only, "
		"not an OpenGL-context test\n";
	return 0;
}

static bool GLFWOpenGLContextProbe(std::string& Version, int& ErrorCode, std::string& ErrorDescription)
{
	s_GLFWProbeError = {};
	glfwSetErrorCallback(CaptureGLFWProbeError);
	if (glfwInit() != GLFW_TRUE)
	{
		ErrorCode = s_GLFWProbeError.Code;
		ErrorDescription = s_GLFWProbeError.Description.data();
		glfwTerminate();
		return false;
	}

	glfwDefaultWindowHints();
	glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
	glfwWindowHint(GLFW_SRGB_CAPABLE, GLFW_FALSE);
	glfwWindowHint(GLFW_DECORATED, GLFW_TRUE);
	glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
	GLFWwindow* Window = glfwCreateWindow(320, 240, "PulseForge raw GLFW NSGL probe", nullptr, nullptr);
	if (!Window)
	{
		ErrorCode = s_GLFWProbeError.Code;
		ErrorDescription = s_GLFWProbeError.Description.data();
		glfwTerminate();
		return false;
	}

	glfwMakeContextCurrent(Window);
	using GlGetString = const unsigned char* (*)(unsigned int);
	auto GetString = reinterpret_cast<GlGetString>(glfwGetProcAddress("glGetString"));
	const unsigned char* VersionString = GetString ? GetString(0x1F02) : nullptr;
	if (VersionString) Version = reinterpret_cast<const char*>(VersionString);
	glfwDestroyWindow(Window);
	glfwTerminate();
	if (Version.empty())
	{
		ErrorCode = GLFW_VERSION_UNAVAILABLE;
		ErrorDescription = "NSGL context did not provide glGetString(GL_VERSION)";
		return false;
	}
	ErrorCode = GLFW_NO_ERROR;
	ErrorDescription.clear();
	return true;
}

static int RunOpenGLContextWithoutVulkanSdk()
{
	const char* ExistingSdkValue = std::getenv("VULKAN_SDK");
	const std::string ExistingSdk = ExistingSdkValue ? ExistingSdkValue : "";
	const bool HadSdkValue = ExistingSdkValue != nullptr;
	if (setenv("VULKAN_SDK", "/path/that/does/not/exist", 1) != 0)
	{
		std::cerr << "Could not select the invalid Vulkan SDK for the OpenGL startup check" << '\n';
		return 1;
	}

	std::cout << "MACOS_OPENGL_REQUEST GLFW=" << GLFW_VERSION_MAJOR << '.' << GLFW_VERSION_MINOR << '.'
		<< GLFW_VERSION_REVISION << "; context API=NATIVE; minimum=1.0; profile=ANY; framebuffer defaults=RGBA8/8/8/8, "
		"depth=24, stencil=8, double-buffered; SRGB=false; client=OPENGL\n";
	std::string ProbeVersion;
	int ProbeError = GLFW_NO_ERROR;
	std::string ProbeDescription;
	const bool RawProbeSucceeded = GLFWOpenGLContextProbe(ProbeVersion, ProbeError, ProbeDescription);
	std::cout << "MACOS_OPENGL_RAW_GLFW_PROBE=" << (RawProbeSucceeded ? "PASS" : "FAIL")
		<< "; error=" << ProbeError << "; description=" << ProbeDescription;
	if (RawProbeSucceeded)
		std::cout << "; version=" << ProbeVersion;
	std::cout << '\n';

	bool EngineWindowSucceeded = false;
	int EngineGLFWError = GLFW_NO_ERROR;
	std::string EngineGLFWDescription;
	try
	{
		PulseForge::GLFWWindow OpenGLWindow(PulseForge::WindowProps(
			"PulseForge OpenGL context without Vulkan SDK", 320, 240, PulseForge::EWindowStyle::Default,
			PulseForge::EWindowClientAPI::OpenGL));
		if (PulseForge::GLFWWindow::GetVulkanLoaderProcAddress())
			throw std::runtime_error("OpenGL startup unexpectedly acquired a Vulkan loader");
		GLFWwindow* NativeWindow = static_cast<GLFWwindow*>(OpenGLWindow.GetNativeWindow());
		glfwMakeContextCurrent(NativeWindow);
		using GlGetString = const unsigned char* (*)(unsigned int);
		auto GetString = reinterpret_cast<GlGetString>(glfwGetProcAddress("glGetString"));
		const unsigned char* VersionString = GetString ? GetString(0x1F02) : nullptr;
		if (!VersionString)
			throw std::runtime_error("OpenGL context did not expose glGetString(GL_VERSION)");
		std::cout << "MACOS_OPENGL_ENGINE_CONTEXT=PASS; version=" << VersionString
			<< "; major=" << glfwGetWindowAttrib(NativeWindow, GLFW_CONTEXT_VERSION_MAJOR)
			<< "; minor=" << glfwGetWindowAttrib(NativeWindow, GLFW_CONTEXT_VERSION_MINOR)
			<< "; profile=" << glfwGetWindowAttrib(NativeWindow, GLFW_OPENGL_PROFILE) << '\n';
		EngineWindowSucceeded = true;
	}
	catch (const std::exception& Error)
	{
		const char* Description = nullptr;
		EngineGLFWError = glfwGetError(&Description);
		EngineGLFWDescription = Description ? Description : "";
		if (std::string_view(Error.what()) != "GLFW window creation failed")
		{
			std::cerr << "PulseForge OpenGL startup failed outside native window creation: " << Error.what() << '\n';
			return 1;
		}
	}

	if (PulseForge::GLFWWindow::GetVulkanLoaderProcAddress())
	{
		std::cerr << "OpenGL startup path retained an unexpected Vulkan loader" << '\n';
		return 1;
	}
	if (EngineWindowSucceeded)
	{
		if (!RawProbeSucceeded)
		{
			std::cerr << "The raw GLFW OpenGL probe failed but the PulseForge window unexpectedly succeeded" << '\n';
			return 1;
		}
		if (PulseForge::GLFWWindow::GetVulkanLoaderProcAddress())
		{
			std::cerr << "OpenGL context teardown retained a Vulkan loader" << '\n';
			return 1;
		}
		if (HadSdkValue)
			setenv("VULKAN_SDK", ExistingSdk.c_str(), 1);
		else
			unsetenv("VULKAN_SDK");
		std::cout << "MACOS_OPENGL_CONTEXT_WITHOUT_VULKAN_SDK=PASS; real PulseForge GLFW context created; "
			"Vulkan SDK was unusable\n";
		return 0;
	}

	const bool ExactNSGLCapabilityFailure = ProbeError == GLFW_FORMAT_UNAVAILABLE &&
		ProbeDescription == "NSGL: Failed to find a suitable pixel format" &&
		EngineGLFWError == GLFW_FORMAT_UNAVAILABLE &&
		EngineGLFWDescription == "NSGL: Failed to find a suitable pixel format";
	if (RawProbeSucceeded || !ExactNSGLCapabilityFailure)
	{
		std::cerr << "OpenGL integration failed without a matching raw GLFW/NSGL capability result; raw="
			<< ProbeError << " (" << ProbeDescription << "), PulseForge=" << EngineGLFWError << " ("
			<< EngineGLFWDescription << ")\n";
		return 1;
	}

	if (HadSdkValue)
		setenv("VULKAN_SDK", ExistingSdk.c_str(), 1);
	else
		unsetenv("VULKAN_SDK");
	if (!CreateAndCloseVulkanWindow("PulseForge Vulkan recovery after NSGL context unavailability")) return 1;
	std::cout << "MACOS_OPENGL_CONTEXT_WITHOUT_VULKAN_SDK=SKIP; capability=GLFW_FORMAT_UNAVAILABLE; "
		"raw GLFW and PulseForge both received the exact NSGL accelerated-pixel-format error; loader state stayed clean "
		"and Vulkan recovered\n";
	return 77;
}

int main(int ArgumentCount, char** Arguments)
{
	try
	{
		PulseForge::Log::Init();
		if (ArgumentCount == 3 && std::string_view(Arguments[1]) == "--invalid-sdk-child")
			return RunExpectedEmptySdkFailure(Arguments[2]);
		if (ArgumentCount == 2 && std::string_view(Arguments[1]) == "--vulkan-lifecycle")
			return RunVulkanLoaderLifecycle(Arguments[0]);
		if (ArgumentCount == 2 && std::string_view(Arguments[1]) == "--runtime-mismatch-policy")
			return RunVulkanLoaderMismatchPolicy();
		if (ArgumentCount == 2 && std::string_view(Arguments[1]) == "--opengl-context-without-vulkan-sdk")
			return RunOpenGLContextWithoutVulkanSdk();
		std::cerr << "Unexpected macOS Vulkan loader lifecycle arguments" << '\n';
		return 1;
	}
	catch (const std::exception& Error)
	{
		std::cerr << "MacOS platform validation threw unexpectedly: " << Error.what() << '\n';
		return 1;
	}
	catch (...)
	{
		std::cerr << "MacOS platform validation threw an unknown exception" << '\n';
		return 1;
	}
}
