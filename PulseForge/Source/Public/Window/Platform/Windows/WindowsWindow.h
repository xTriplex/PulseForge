#pragma once

#include "Window/Window.h"

struct GLFWwindow;

namespace PulseForge
{
	class WindowsWindow : public Window
	{
	public:
		WindowsWindow(const WindowProps& Props);

		virtual ~WindowsWindow();

		virtual void PollEvents() override;
		virtual void WaitEventsTimeout(double TimeoutSeconds) override;

		virtual unsigned int GetWidth() const override { return WindowData.Width; }
		virtual unsigned int GetHeight() const override { return WindowData.Height; }
		virtual std::pair<unsigned int, unsigned int> GetFramebufferSize() const override;

		virtual void SetEventCallback(const EventCallbackFn& Callback) override { WindowData.EventCallback = Callback; }

		virtual void* GetNativeWindow() const override { return m_NativeWindow; }

	private:
		virtual void Init(const WindowProps& Props);
		virtual void Shutdown();

	private:
		GLFWwindow* m_NativeWindow = nullptr;
		bool m_GLFWRuntimeAcquired = false;

		struct FWindowData
		{
			std::string Title;
			unsigned int Width = 0;
			unsigned int Height = 0;
			EventCallbackFn EventCallback;
		};

		FWindowData WindowData;
	};
}
