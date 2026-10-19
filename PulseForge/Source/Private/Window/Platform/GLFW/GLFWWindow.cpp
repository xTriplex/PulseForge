#include "Core/Log.h"
#include "Window/Platform/GLFW/GLFWWindow.h"
#include "Events/ApplicationEvent.h"
#include "Events/KeyEvent.h"
#include "Events/MouseEvent.h"

#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>

#if defined(__APPLE__)
	#include <dlfcn.h>
	#include <mach-o/dyld.h>
	#include <vulkan/vulkan.h>
#endif

#include <GLFW/glfw3.h>

namespace PulseForge
{
	static unsigned int s_GLFWWindowCount = 0;
	#if defined(__APPLE__)
	static void* s_VulkanLoaderLibrary = nullptr;
	static PFN_vkGetInstanceProcAddr s_VulkanLoaderProcAddress = nullptr;
	static bool s_GLFWInitUsesVulkanLoader = false;

	struct ScopedMacOSVulkanLibrary
	{
		explicit ScopedMacOSVulkanLibrary(void* Library) noexcept : Handle(Library) {}
		~ScopedMacOSVulkanLibrary() { if (Handle) dlclose(Handle); }
		ScopedMacOSVulkanLibrary(const ScopedMacOSVulkanLibrary&) = delete;
		ScopedMacOSVulkanLibrary& operator=(const ScopedMacOSVulkanLibrary&) = delete;

		[[nodiscard]] void* Release() noexcept
		{
			void* Library = Handle;
			Handle = nullptr;
			return Library;
		}

		void* Handle = nullptr;
	};

	static void ResetMacOSVulkanLoader() noexcept
	{
		glfwInitVulkanLoader(nullptr);
		s_VulkanLoaderProcAddress = nullptr;
		if (s_VulkanLoaderLibrary)
		{
			dlclose(s_VulkanLoaderLibrary);
			s_VulkanLoaderLibrary = nullptr;
		}
		s_GLFWInitUsesVulkanLoader = false;
	}

	static bool TryLoadMacOSVulkanLoader(
		const std::filesystem::path& Path,
		bool RequireRequestedImage,
		std::string& Diagnostic)
	{
		(void)dlerror();
		ScopedMacOSVulkanLibrary Library(dlopen(Path.c_str(), RTLD_NOW | RTLD_LOCAL));
		if (!Library.Handle)
		{
			const char* LoaderError = dlerror();
			Diagnostic = LoaderError ? LoaderError : "dlopen failed without a diagnostic";
			return false;
		}

		(void)dlerror();
		PFN_vkGetInstanceProcAddr ProcAddress = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
			dlsym(Library.Handle, "vkGetInstanceProcAddr"));
		const char* SymbolError = dlerror();
		if (!ProcAddress || SymbolError)
		{
			Diagnostic = SymbolError ? SymbolError : "the loader did not export vkGetInstanceProcAddr";
			return false;
		}

		if (RequireRequestedImage)
		{
			Dl_info Image{};
			if (!dladdr(reinterpret_cast<const void*>(ProcAddress), &Image) || !Image.dli_fname)
			{
				Diagnostic = "dladdr could not identify the image containing vkGetInstanceProcAddr";
				return false;
			}

			const std::string LoadedImagePath = Image.dli_fname;
			std::error_code RequestedPathError;
			std::error_code LoadedPathError;
			const std::filesystem::path RequestedImage = std::filesystem::weakly_canonical(Path, RequestedPathError);
			const std::filesystem::path LoadedImage = std::filesystem::weakly_canonical(LoadedImagePath, LoadedPathError);
			if (RequestedPathError || LoadedPathError)
			{
				Diagnostic = "could not canonicalize requested image '" + Path.string() + "' or loaded image '" +
					LoadedImagePath + "'";
				return false;
			}
			if (RequestedImage != LoadedImage)
			{
				Diagnostic = "requested image '" + Path.string() + "' resolved to '" + LoadedImagePath +
					"' instead; DYLD_LIBRARY_PATH may have selected a different Vulkan loader";
				return false;
			}
		}

		s_VulkanLoaderLibrary = Library.Release();
		s_VulkanLoaderProcAddress = ProcAddress;
		Diagnostic.clear();
		return true;
	}

	static void InitializeMacOSVulkanLoader()
	{
		if (const char* VulkanSdk = std::getenv("VULKAN_SDK"); VulkanSdk && VulkanSdk[0] != '\0')
		{
			const std::filesystem::path LibraryDirectory = std::filesystem::path(VulkanSdk) / "lib";
			std::string VersionedLoaderDiagnostic;
			std::string UnversionedLoaderDiagnostic;
			if (!TryLoadMacOSVulkanLoader(
					LibraryDirectory / "libvulkan.1.dylib", true, VersionedLoaderDiagnostic) &&
				!TryLoadMacOSVulkanLoader(
					LibraryDirectory / "libvulkan.dylib", true, UnversionedLoaderDiagnostic))
				throw std::runtime_error(
					"VULKAN_SDK is set but no loader was loaded from that SDK. libvulkan.1.dylib: " +
					VersionedLoaderDiagnostic + "; libvulkan.dylib: " + UnversionedLoaderDiagnostic);
		}
		else
		{
			uint32_t PathLength = 0;
			(void)_NSGetExecutablePath(nullptr, &PathLength);
			std::string ExecutablePath(PathLength, '\0');
			if (PathLength > 0 && _NSGetExecutablePath(ExecutablePath.data(), &PathLength) == 0)
			{
				const std::filesystem::path BundleLoader =
					std::filesystem::weakly_canonical(std::filesystem::path(ExecutablePath.c_str())).parent_path() /
					"../Frameworks/libvulkan.1.dylib";
				std::string IgnoredDiagnostic;
				(void)TryLoadMacOSVulkanLoader(BundleLoader, false, IgnoredDiagnostic);
			}
			std::string IgnoredDiagnostic;
			if (!s_VulkanLoaderProcAddress && !TryLoadMacOSVulkanLoader("libvulkan.1.dylib", false, IgnoredDiagnostic))
				(void)TryLoadMacOSVulkanLoader("libvulkan.dylib", false, IgnoredDiagnostic);
			if (!s_VulkanLoaderProcAddress)
				throw std::runtime_error("No Vulkan loader found in the app bundle or the macOS loader search path");
		}

		glfwInitVulkanLoader(s_VulkanLoaderProcAddress);
		s_GLFWInitUsesVulkanLoader = true;
	}
	#endif

	static void GLFWErrorCallback(int Error, const char* Description)
	{
		PF_CORE_ERROR("GLFW Error ({0}): {1}", Error, Description);
	}

	GLFWWindow::GLFWWindow(const WindowProps& Props)
	{
		Init(Props);
	}

	GLFWWindow::~GLFWWindow()
	{
		Shutdown();
	}

	void GLFWWindow::Init(const WindowProps& Props)
	{
		WindowData.Title = Props.m_Title;
		WindowData.Width = Props.m_Width;
		WindowData.Height = Props.m_Height;

		PF_CORE_INFO("Creating window {0} ({1} x {2})", Props.m_Title, Props.m_Width, Props.m_Height);

		if (s_GLFWWindowCount == 0)
		{
			glfwSetErrorCallback(GLFWErrorCallback);
			#if defined(__APPLE__)
			if (Props.m_ClientAPI == EWindowClientAPI::Vulkan)
				InitializeMacOSVulkanLoader();
			else
				glfwInitVulkanLoader(nullptr);
			#endif
			if (glfwInit() != GLFW_TRUE)
			{
				#if defined(__APPLE__)
				if (s_GLFWInitUsesVulkanLoader || s_VulkanLoaderLibrary) ResetMacOSVulkanLoader();
				#endif
				PF_CORE_ERROR("Could not initialize GLFW");
				throw std::runtime_error("GLFW initialization failed");
			}
		}
		#if defined(__APPLE__)
		else if (Props.m_ClientAPI == EWindowClientAPI::Vulkan && !s_GLFWInitUsesVulkanLoader)
			throw std::runtime_error("A Vulkan window cannot join a GLFW runtime initialized without its Vulkan loader");
		#endif

		++s_GLFWWindowCount;
		m_GLFWRuntimeAcquired = true;
		try
		{
			// Apply styles before window creation!
			glfwDefaultWindowHints();
			glfwWindowHint(GLFW_CLIENT_API, Props.m_ClientAPI == EWindowClientAPI::OpenGL ? GLFW_OPENGL_API : GLFW_NO_API);
			if (Props.m_ClientAPI == EWindowClientAPI::OpenGL) glfwWindowHint(GLFW_SRGB_CAPABLE, GLFW_FALSE);

			if (HasStyle(Props.m_Style, EWindowStyle::Borderless))
			{
				glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
			}

			else
			{
				glfwWindowHint(GLFW_DECORATED, HasStyle(Props.m_Style, EWindowStyle::Titlebar) ? GLFW_TRUE : GLFW_FALSE);
			}

			glfwWindowHint(GLFW_RESIZABLE, HasStyle(Props.m_Style, EWindowStyle::Resize) ? GLFW_TRUE : GLFW_FALSE);

			GLFWmonitor* PrimaryMonitor = HasStyle(Props.m_Style, EWindowStyle::Fullscreen) ? glfwGetPrimaryMonitor() : nullptr;
			m_NativeWindow = glfwCreateWindow((int)Props.m_Width, (int)Props.m_Height, WindowData.Title.c_str(), PrimaryMonitor, nullptr);
			if (!m_NativeWindow)
			{
				PF_CORE_ERROR("Failed to create GLFW window '{0}'", WindowData.Title);
				throw std::runtime_error("GLFW window creation failed");
			}

			glfwSetWindowUserPointer(m_NativeWindow, &WindowData);

			// =========================================
			// GLFW Callbacks
			// =========================================

			// 1. Window Resize
			glfwSetWindowSizeCallback(m_NativeWindow,
				[](GLFWwindow* CurrentWindow, int Width, int Height)
				{
					FWindowData& Data = *(FWindowData*)glfwGetWindowUserPointer(CurrentWindow);
					Data.Width = Width;
					Data.Height = Height;

					WindowResizeEvent Event(Width, Height);
					Data.EventCallback(Event);
				});

			// 2. Window Close
			glfwSetWindowCloseCallback(m_NativeWindow,
				[](GLFWwindow* CurrentWindow)
				{
					FWindowData& Data = *(FWindowData*)glfwGetWindowUserPointer(CurrentWindow);

					WindowCloseEvent Event;
					Data.EventCallback(Event);
				});

			// 3. Window Focus
			glfwSetWindowFocusCallback(m_NativeWindow,
				[](GLFWwindow* CurrentWindow, int Focused)
				{
					FWindowData& Data = *(FWindowData*)glfwGetWindowUserPointer(CurrentWindow);
					if (Focused)
					{
						WindowFocusEvent Event;
						Data.EventCallback(Event);
					}

					else
					{
						WindowLostFocusEvent Event;
						Data.EventCallback(Event);
					}
				});

			// 4. Window Moved
			glfwSetWindowPosCallback(m_NativeWindow,
				[](GLFWwindow* CurrentWindow, int XPos, int YPos)
				{
					FWindowData& Data = *(FWindowData*)glfwGetWindowUserPointer(CurrentWindow);

					WindowMovedEvent Event(XPos, YPos);
					Data.EventCallback(Event);
				});

			// 5. Keyboard Events
			glfwSetKeyCallback(m_NativeWindow,
				[](GLFWwindow* CurrentWindow, int Key, int Scancode, int Action, int Mods)
				{
					FWindowData& Data = *(FWindowData*)glfwGetWindowUserPointer(CurrentWindow);
					switch (Action)
					{
					case GLFW_PRESS:
					{
						KeyPressedEvent Event(Key, 0);
						Data.EventCallback(Event);
						break;
					}
					case GLFW_RELEASE:
					{
						KeyReleasedEvent Event(Key);
						Data.EventCallback(Event);
						break;
					}
					case GLFW_REPEAT:
					{
						KeyPressedEvent Event(Key, 1);
						Data.EventCallback(Event);
						break;
					}
					}
				});

			glfwSetCharCallback(m_NativeWindow,
				[](GLFWwindow* CurrentWindow, unsigned int KeyCode)
				{
					FWindowData& Data = *(FWindowData*)glfwGetWindowUserPointer(CurrentWindow);

					KeyTypedEvent Event(KeyCode);
					Data.EventCallback(Event);
				});

			// 6. Mouse Events
			glfwSetMouseButtonCallback(m_NativeWindow,
				[](GLFWwindow* CurrentWindow, int Button, int Action, int Mods)
				{
					FWindowData& Data = *(FWindowData*)glfwGetWindowUserPointer(CurrentWindow);

					switch (Action)
					{
					case GLFW_PRESS:
					{
						MouseButtonPressedEvent Event(Button);
						Data.EventCallback(Event);
						break;
					}
					case GLFW_RELEASE:
					{
						MouseButtonReleasedEvent Event(Button);
						Data.EventCallback(Event);
						break;
					}
					}
				});

			glfwSetScrollCallback(m_NativeWindow,
				[](GLFWwindow* CurrentWindow, double XOffset, double YOffset)
				{
					FWindowData& Data = *(FWindowData*)glfwGetWindowUserPointer(CurrentWindow);

					MouseScrolledEvent Event((float)XOffset, (float)YOffset);
					Data.EventCallback(Event);
				});

			glfwSetCursorPosCallback(m_NativeWindow,
				[](GLFWwindow* CurrentWindow, double XPos, double YPos)
				{
					FWindowData& Data = *(FWindowData*)glfwGetWindowUserPointer(CurrentWindow);

					MouseMovedEvent Event((float)XPos, (float)YPos);
					Data.EventCallback(Event);
				});
		}
		catch (...)
		{
			Shutdown();
			throw;
		}
	}

	void GLFWWindow::Shutdown()
	{
		if (m_NativeWindow)
		{
			glfwDestroyWindow(m_NativeWindow);
			m_NativeWindow = nullptr;
		}

		if (m_GLFWRuntimeAcquired)
		{
			m_GLFWRuntimeAcquired = false;
			if (--s_GLFWWindowCount == 0)
			{
				glfwTerminate();
				#if defined(__APPLE__)
				ResetMacOSVulkanLoader();
				#endif
			}
		}
	}

	GLFWWindow::VulkanLoaderProcAddress GLFWWindow::GetVulkanLoaderProcAddress() noexcept
	{
		#if defined(__APPLE__)
		return reinterpret_cast<VulkanLoaderProcAddress>(s_VulkanLoaderProcAddress);
		#else
		return nullptr;
		#endif
	}

	void GLFWWindow::PollEvents()
	{
		glfwPollEvents();
	}

	void GLFWWindow::WaitEventsTimeout(double TimeoutSeconds)
	{
		glfwWaitEventsTimeout(TimeoutSeconds);
	}

	std::pair<unsigned int, unsigned int> GLFWWindow::GetFramebufferSize() const
	{
		int Width = 0;
		int Height = 0;
		glfwGetFramebufferSize(m_NativeWindow, &Width, &Height);
		return { static_cast<unsigned int>(Width), static_cast<unsigned int>(Height) };
	}

	std::pair<float, float> GLFWWindow::GetContentScale() const
	{
		float XScale = 1.0f;
		float YScale = 1.0f;
		glfwGetWindowContentScale(m_NativeWindow, &XScale, &YScale);
		return { XScale, YScale };
	}

} // namespace PulseForge
