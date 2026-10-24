#pragma once

#include "Window/Window.h"
#include "Input/GamepadStateAdapter.h"

#include <array>

struct GLFWwindow;

namespace PulseForge
{
	class GLFWWindow : public Window
	{
	public:
		using VulkanLoaderProcAddress = void (*)();

		GLFWWindow(const WindowProps& Props);

		virtual ~GLFWWindow();

		virtual void PollEvents() override;
		virtual void WaitEventsTimeout(double TimeoutSeconds) override;

		virtual unsigned int GetWidth() const override { return WindowData.Width; }
		virtual unsigned int GetHeight() const override { return WindowData.Height; }
		virtual std::pair<unsigned int, unsigned int> GetFramebufferSize() const override;
		virtual std::pair<float, float> GetContentScale() const override;

		virtual void SetEventCallback(const EventCallbackFn& Callback) override
		{
			WindowData.EventCallback = Callback ? Callback : EventCallbackFn([](Event&) {});
		}
		virtual void SetInputEventCallback(const InputCallbackFn& Callback) override
		{
			WindowData.InputCallback = Callback ? Callback : InputCallbackFn([](const InputEvent&) {});
		}
		[[nodiscard]] virtual InputWindowID GetInputWindowID() const noexcept override { return WindowData.ID; }
		[[nodiscard]] InputWindowID RegisterInputWindow(void* NativeWindow);
		void UnregisterInputWindow(void* NativeWindow) noexcept;

		virtual void* GetNativeWindow() const override { return m_NativeWindow; }
		[[nodiscard]] static VulkanLoaderProcAddress GetVulkanLoaderProcAddress() noexcept;

	private:
		virtual void Init(const WindowProps& Props);
		virtual void Shutdown();

	private:
		GLFWwindow* m_NativeWindow = nullptr;
		bool m_GLFWRuntimeAcquired = false;
		GamepadStateAdapter m_GamepadStateAdapter;
		std::array<bool, GamepadStateAdapter::MaximumSlots> m_GamepadPresent{};
		std::array<bool, GamepadStateAdapter::MaximumSlots> m_GamepadMapped{};
		void PollGamepads() noexcept;
		static void SecondaryWindowFocusCallback(GLFWwindow* Window, int Focused);
		static void SecondaryWindowKeyCallback(GLFWwindow* Window, int Key, int Scancode, int Action, int Mods);
		static void SecondaryWindowCharCallback(GLFWwindow* Window, unsigned int Character);
		static void SecondaryWindowMouseButtonCallback(GLFWwindow* Window, int Button, int Action, int Mods);
		static void SecondaryWindowScrollCallback(GLFWwindow* Window, double XOffset, double YOffset);
		static void SecondaryWindowCursorPosCallback(GLFWwindow* Window, double X, double Y);

		struct FWindowData
		{
			std::string Title;
			InputWindowID ID = 0;
			unsigned int Width = 0;
			unsigned int Height = 0;
			EventCallbackFn EventCallback = [](Event&) {};
			InputCallbackFn InputCallback = [](const InputEvent&) {};
		};

		FWindowData WindowData;
	};
} // namespace PulseForge
