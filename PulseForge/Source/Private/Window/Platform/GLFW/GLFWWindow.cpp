#include "Core/Log.h"
#include "Window/Platform/GLFW/GLFWWindow.h"
#include "Events/ApplicationEvent.h"
#include "Events/KeyEvent.h"
#include "Events/MouseEvent.h"

#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>

#if defined(__APPLE__)
	#include <dlfcn.h>
	#include <mach-o/dyld.h>
	#include <vulkan/vulkan.h>
#endif

#include <GLFW/glfw3.h>

namespace PulseForge
{
	static unsigned int s_GLFWWindowCount = 0;
	static InputWindowID s_NextInputWindowID = 1;
	struct SecondaryInputCallbacks
	{
		GLFWWindow* Owner = nullptr;
		InputWindowID ID = 0;
		GLFWwindowfocusfun Focus = nullptr;
		GLFWkeyfun Key = nullptr;
		GLFWcharfun Character = nullptr;
		GLFWmousebuttonfun MouseButton = nullptr;
		GLFWscrollfun Scroll = nullptr;
		GLFWcursorposfun CursorPosition = nullptr;
	};
	static std::unordered_map<GLFWwindow*, SecondaryInputCallbacks> s_SecondaryInputCallbacks;
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

	static uint16_t ConvertModifiers(int Modifiers) noexcept
	{
		uint16_t Result = 0;
		if (Modifiers & GLFW_MOD_SHIFT) Result |= static_cast<uint16_t>(InputModifier::Shift);
		if (Modifiers & GLFW_MOD_CONTROL) Result |= static_cast<uint16_t>(InputModifier::Control);
		if (Modifiers & GLFW_MOD_ALT) Result |= static_cast<uint16_t>(InputModifier::Alt);
		if (Modifiers & GLFW_MOD_SUPER) Result |= static_cast<uint16_t>(InputModifier::Super);
		if (Modifiers & GLFW_MOD_CAPS_LOCK) Result |= static_cast<uint16_t>(InputModifier::CapsLock);
		if (Modifiers & GLFW_MOD_NUM_LOCK) Result |= static_cast<uint16_t>(InputModifier::NumLock);
		return Result;
	}

	static uint64_t MakeGamepadInstance(uint32_t Slot, uint64_t Generation) noexcept
	{
		return (Generation << 8) | (static_cast<uint64_t>(Slot) + 1);
	}

	GLFWWindow::GLFWWindow(const WindowProps& Props)
	{
		Init(Props);
	}

	GLFWWindow::~GLFWWindow()
	{
		Shutdown();
	}

	InputWindowID GLFWWindow::RegisterInputWindow(void* NativeWindow)
	{
		GLFWwindow* Window = static_cast<GLFWwindow*>(NativeWindow);
		if (!Window || Window == m_NativeWindow || s_SecondaryInputCallbacks.contains(Window)) return 0;

		SecondaryInputCallbacks Callbacks;
		Callbacks.Owner = this;
		Callbacks.ID = s_NextInputWindowID++;
		try
		{
			s_SecondaryInputCallbacks.emplace(Window, Callbacks);
			auto& Registered = s_SecondaryInputCallbacks.at(Window);
			Registered.Focus = glfwSetWindowFocusCallback(Window, &GLFWWindow::SecondaryWindowFocusCallback);
			Registered.Key = glfwSetKeyCallback(Window, &GLFWWindow::SecondaryWindowKeyCallback);
			Registered.Character = glfwSetCharCallback(Window, &GLFWWindow::SecondaryWindowCharCallback);
			Registered.MouseButton = glfwSetMouseButtonCallback(Window, &GLFWWindow::SecondaryWindowMouseButtonCallback);
			Registered.Scroll = glfwSetScrollCallback(Window, &GLFWWindow::SecondaryWindowScrollCallback);
			Registered.CursorPosition = glfwSetCursorPosCallback(Window, &GLFWWindow::SecondaryWindowCursorPosCallback);
			Callbacks = Registered;
		}
		catch (...)
		{
			const auto Registered = s_SecondaryInputCallbacks.find(Window);
			if (Registered != s_SecondaryInputCallbacks.end())
			{
				glfwSetWindowFocusCallback(Window, Registered->second.Focus);
				glfwSetKeyCallback(Window, Registered->second.Key);
				glfwSetCharCallback(Window, Registered->second.Character);
				glfwSetMouseButtonCallback(Window, Registered->second.MouseButton);
				glfwSetScrollCallback(Window, Registered->second.Scroll);
				glfwSetCursorPosCallback(Window, Registered->second.CursorPosition);
				s_SecondaryInputCallbacks.erase(Registered);
			}
			throw;
		}
		return Callbacks.ID;
	}

	void GLFWWindow::UnregisterInputWindow(void* NativeWindow) noexcept
	{
		GLFWwindow* Window = static_cast<GLFWwindow*>(NativeWindow);
		const auto Registered = s_SecondaryInputCallbacks.find(Window);
		if (Registered == s_SecondaryInputCallbacks.end() || Registered->second.Owner != this) return;
		const SecondaryInputCallbacks Callbacks = Registered->second;
		if (Window)
		{
			glfwSetWindowFocusCallback(Window, Callbacks.Focus);
			glfwSetKeyCallback(Window, Callbacks.Key);
			glfwSetCharCallback(Window, Callbacks.Character);
			glfwSetMouseButtonCallback(Window, Callbacks.MouseButton);
			glfwSetScrollCallback(Window, Callbacks.Scroll);
			glfwSetCursorPosCallback(Window, Callbacks.CursorPosition);
		}
		s_SecondaryInputCallbacks.erase(Registered);
		WindowData.InputCallback({ .Kind = InputEventKind::WindowDestroyed, .Window = Callbacks.ID });
	}

	void GLFWWindow::SecondaryWindowFocusCallback(GLFWwindow* Window, int Focused)
	{
		const auto Found = s_SecondaryInputCallbacks.find(Window);
		if (Found == s_SecondaryInputCallbacks.end()) return;
		const SecondaryInputCallbacks Callbacks = Found->second;
		if (Callbacks.Focus) Callbacks.Focus(Window, Focused);
		if (Focused)
		{
			WindowFocusEvent Event;
			Event.SetInputWindowID(Callbacks.ID);
			Callbacks.Owner->WindowData.EventCallback(Event);
		}
		else
		{
			WindowLostFocusEvent Event;
			Event.SetInputWindowID(Callbacks.ID);
			Callbacks.Owner->WindowData.EventCallback(Event);
		}
	}

	void GLFWWindow::SecondaryWindowKeyCallback(GLFWwindow* Window, int Key, int Scancode, int Action, int Mods)
	{
		const auto Found = s_SecondaryInputCallbacks.find(Window);
		if (Found == s_SecondaryInputCallbacks.end()) return;
		const SecondaryInputCallbacks Callbacks = Found->second;
		if (Callbacks.Key) Callbacks.Key(Window, Key, Scancode, Action, Mods);
		switch (Action)
		{
		case GLFW_PRESS:
		{
			KeyPressedEvent Event(Key, 0, ConvertModifiers(Mods));
			Event.SetInputWindowID(Callbacks.ID);
			Callbacks.Owner->WindowData.EventCallback(Event);
			break;
		}
		case GLFW_RELEASE:
		{
			KeyReleasedEvent Event(Key, ConvertModifiers(Mods));
			Event.SetInputWindowID(Callbacks.ID);
			Callbacks.Owner->WindowData.EventCallback(Event);
			break;
		}
		case GLFW_REPEAT:
		{
			KeyPressedEvent Event(Key, 1, ConvertModifiers(Mods));
			Event.SetInputWindowID(Callbacks.ID);
			Callbacks.Owner->WindowData.EventCallback(Event);
			break;
		}
		}
	}

	void GLFWWindow::SecondaryWindowCharCallback(GLFWwindow* Window, unsigned int Character)
	{
		const auto Found = s_SecondaryInputCallbacks.find(Window);
		if (Found == s_SecondaryInputCallbacks.end()) return;
		const SecondaryInputCallbacks Callbacks = Found->second;
		if (Callbacks.Character) Callbacks.Character(Window, Character);
		KeyTypedEvent Event(static_cast<int>(Character));
		Event.SetInputWindowID(Callbacks.ID);
		Callbacks.Owner->WindowData.EventCallback(Event);
	}

	void GLFWWindow::SecondaryWindowMouseButtonCallback(GLFWwindow* Window, int Button, int Action, int Mods)
	{
		const auto Found = s_SecondaryInputCallbacks.find(Window);
		if (Found == s_SecondaryInputCallbacks.end()) return;
		const SecondaryInputCallbacks Callbacks = Found->second;
		if (Callbacks.MouseButton) Callbacks.MouseButton(Window, Button, Action, Mods);
		if (Action == GLFW_PRESS)
		{
			MouseButtonPressedEvent Event(Button, ConvertModifiers(Mods));
			Event.SetInputWindowID(Callbacks.ID);
			Callbacks.Owner->WindowData.EventCallback(Event);
		}
		else if (Action == GLFW_RELEASE)
		{
			MouseButtonReleasedEvent Event(Button, ConvertModifiers(Mods));
			Event.SetInputWindowID(Callbacks.ID);
			Callbacks.Owner->WindowData.EventCallback(Event);
		}
	}

	void GLFWWindow::SecondaryWindowScrollCallback(GLFWwindow* Window, double XOffset, double YOffset)
	{
		const auto Found = s_SecondaryInputCallbacks.find(Window);
		if (Found == s_SecondaryInputCallbacks.end()) return;
		const SecondaryInputCallbacks Callbacks = Found->second;
		if (Callbacks.Scroll) Callbacks.Scroll(Window, XOffset, YOffset);
		MouseScrolledEvent Event(static_cast<float>(XOffset), static_cast<float>(YOffset));
		Event.SetInputWindowID(Callbacks.ID);
		Callbacks.Owner->WindowData.EventCallback(Event);
	}

	void GLFWWindow::SecondaryWindowCursorPosCallback(GLFWwindow* Window, double X, double Y)
	{
		const auto Found = s_SecondaryInputCallbacks.find(Window);
		if (Found == s_SecondaryInputCallbacks.end()) return;
		const SecondaryInputCallbacks Callbacks = Found->second;
		if (Callbacks.CursorPosition) Callbacks.CursorPosition(Window, X, Y);
		MouseMovedEvent Event(static_cast<float>(X), static_cast<float>(Y));
		Event.SetInputWindowID(Callbacks.ID);
		Callbacks.Owner->WindowData.EventCallback(Event);
	}

	void GLFWWindow::Init(const WindowProps& Props)
	{
		WindowData.Title = Props.m_Title;
		WindowData.ID = s_NextInputWindowID++;
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
						Event.SetInputWindowID(Data.ID);
						Data.EventCallback(Event);
					}

					else
					{
						WindowLostFocusEvent Event;
						Event.SetInputWindowID(Data.ID);
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
						KeyPressedEvent Event(Key, 0, ConvertModifiers(Mods));
						Event.SetInputWindowID(Data.ID);
						Data.EventCallback(Event);
						break;
					}
					case GLFW_RELEASE:
					{
						KeyReleasedEvent Event(Key, ConvertModifiers(Mods));
						Event.SetInputWindowID(Data.ID);
						Data.EventCallback(Event);
						break;
					}
					case GLFW_REPEAT:
					{
						KeyPressedEvent Event(Key, 1, ConvertModifiers(Mods));
						Event.SetInputWindowID(Data.ID);
						Data.EventCallback(Event);
						break;
					}
					}
				});

			glfwSetCharCallback(m_NativeWindow,
				[](GLFWwindow* CurrentWindow, unsigned int KeyCode)
				{
					FWindowData& Data = *(FWindowData*)glfwGetWindowUserPointer(CurrentWindow);

					KeyTypedEvent Event(static_cast<int>(KeyCode));
					Event.SetInputWindowID(Data.ID);
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
						MouseButtonPressedEvent Event(Button, ConvertModifiers(Mods));
						Event.SetInputWindowID(Data.ID);
						Data.EventCallback(Event);
						break;
					}
					case GLFW_RELEASE:
					{
						MouseButtonReleasedEvent Event(Button, ConvertModifiers(Mods));
						Event.SetInputWindowID(Data.ID);
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
					Event.SetInputWindowID(Data.ID);
					Data.EventCallback(Event);
				});

			glfwSetCursorPosCallback(m_NativeWindow,
				[](GLFWwindow* CurrentWindow, double XPos, double YPos)
				{
					FWindowData& Data = *(FWindowData*)glfwGetWindowUserPointer(CurrentWindow);

					MouseMovedEvent Event((float)XPos, (float)YPos);
					Event.SetInputWindowID(Data.ID);
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
			for (uint32_t Slot = 0; Slot < m_GamepadConnected.size(); ++Slot)
			{
				if (!m_GamepadConnected[Slot]) continue;
				WindowData.InputCallback({ .Kind = InputEventKind::DeviceDisconnected,
					.DeviceInstance = MakeGamepadInstance(Slot, m_GamepadGenerations[Slot]) });
				m_GamepadConnected[Slot] = false;
			}
			WindowData.InputCallback({ .Kind = InputEventKind::WindowDestroyed, .Window = WindowData.ID });
		}
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
		PollGamepads();
	}

	void GLFWWindow::PollGamepads() noexcept
	{
		for (uint32_t Slot = 0; Slot < m_GamepadConnected.size(); ++Slot)
		{
			const int Joystick = GLFW_JOYSTICK_1 + static_cast<int>(Slot);
			const bool Present = glfwJoystickPresent(Joystick) == GLFW_TRUE;
			const bool Mapped = Present && glfwJoystickIsGamepad(Joystick) == GLFW_TRUE;
			if (!Present)
			{
				if (m_GamepadConnected[Slot])
					WindowData.InputCallback({ .Kind = InputEventKind::DeviceDisconnected,
						.DeviceInstance = MakeGamepadInstance(Slot, m_GamepadGenerations[Slot]) });
				m_GamepadConnected[Slot] = false;
				m_GamepadMapped[Slot] = false;
				m_GamepadAxes[Slot].fill(0.0f);
				m_GamepadButtons[Slot].fill(false);
				continue;
			}

			if (!m_GamepadConnected[Slot] || m_GamepadMapped[Slot] != Mapped)
			{
				if (m_GamepadConnected[Slot])
					WindowData.InputCallback({ .Kind = InputEventKind::DeviceDisconnected,
						.DeviceInstance = MakeGamepadInstance(Slot, m_GamepadGenerations[Slot]) });
				++m_GamepadGenerations[Slot];
				m_GamepadConnected[Slot] = true;
				m_GamepadMapped[Slot] = Mapped;
				m_GamepadAxes[Slot].fill(0.0f);
				m_GamepadButtons[Slot].fill(false);
				if (Mapped)
					WindowData.InputCallback({ .Kind = InputEventKind::DeviceConnected,
						.DeviceInstance = MakeGamepadInstance(Slot, m_GamepadGenerations[Slot]) });
				else
				{
					const char* Name = glfwGetJoystickName(Joystick);
					PF_CORE_WARN("Gamepad '{0}' is unavailable because GLFW has no supported mapping",
						Name ? Name : "Unknown controller");
				}
			}
			if (!Mapped) continue;

			GLFWgamepadstate State{};
			if (glfwGetGamepadState(Joystick, &State) != GLFW_TRUE) continue;
			const uint64_t DeviceInstance = MakeGamepadInstance(Slot, m_GamepadGenerations[Slot]);
			for (uint16_t Button = 0; Button < m_GamepadButtons[Slot].size(); ++Button)
			{
				const bool Down = State.buttons[Button] == GLFW_PRESS;
				if (Down == m_GamepadButtons[Slot][Button]) continue;
				m_GamepadButtons[Slot][Button] = Down;
				WindowData.InputCallback({ .Kind = Down ? InputEventKind::Pressed : InputEventKind::Released,
					.Control = { InputDevice::GamepadButton, Button }, .DeviceInstance = DeviceInstance });
			}
			for (uint16_t Axis = 0; Axis < m_GamepadAxes[Slot].size(); ++Axis)
			{
				float Value = std::clamp(State.axes[Axis], -1.0f, 1.0f);
				if (!std::isfinite(Value)) Value = 0.0f;
				if (Axis >= static_cast<uint16_t>(GamepadAxis::LeftTrigger)) Value = (Value + 1.0f) * 0.5f;
				if (std::abs(Value - m_GamepadAxes[Slot][Axis]) < 0.0001f) continue;
				m_GamepadAxes[Slot][Axis] = Value;
				WindowData.InputCallback({ .Kind = InputEventKind::Value,
					.Control = { InputDevice::GamepadAxis, Axis }, .DeviceInstance = DeviceInstance, .X = Value });
			}
		}
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
