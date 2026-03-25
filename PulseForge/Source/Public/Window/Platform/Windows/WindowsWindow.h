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

		virtual void OnUpdate() override;

		virtual unsigned int GetWidth() const override { return WindowData.Width; }
		virtual unsigned int GetHeight() const override { return WindowData.Height; }

		virtual void SetEventCallback(const EventCallbackFn& Callback) override { WindowData.EventCallback = Callback; }

		virtual void SetVSync(bool bEnabled) override;
		virtual bool IsVSync() const override;

		virtual void* GetNativeWindow() const override { return m_NativeWindow; }

	private:
		virtual void Init(const WindowProps& Props);
		virtual void Shutdown();

	private:
		GLFWwindow* m_NativeWindow;

		struct FWindowData
		{
			std::string Title;
			unsigned int Width = 0;
			unsigned int Height = 0;
			bool bVSync = false;

			EventCallbackFn EventCallback;
		};

		FWindowData WindowData;
	};
}
