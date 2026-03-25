#include "pfpch.h"
#include "Window/Platform/Windows/WindowsWindow.h"
#include "Events/ApplicationEvent.h"
#include "Events/KeyEvent.h"
#include "Events/MouseEvent.h"

#include <glad/glad.h>
#include <GLFW/glfw3.h>

namespace PulseForge
{
	static bool bGLFWInitialized = false;

	static void GLFWErrorCallback(int Error, const char* Description)
	{
		PF_CORE_ERROR("GLFW Error ({0}): {1}", Error, Description);
	}

	WindowsWindow::WindowsWindow(const WindowProps& Props)
	{
		Init(Props);
	}

	WindowsWindow::~WindowsWindow()
	{
		Shutdown();
	}

	void WindowsWindow::Init(const WindowProps& Props)
	{
		WindowData.Title = Props.m_Title;
		WindowData.Width = Props.m_Width;
		WindowData.Height = Props.m_Height;

		PF_CORE_INFO("Creating window {0} ({1} x {2})", Props.m_Title, Props.m_Width, Props.m_Height);

		if (!bGLFWInitialized)
		{
			int Success = glfwInit();
			PF_CORE_ASSERT(Success, "Could not initialize GLFW!");
			glfwSetErrorCallback(GLFWErrorCallback);
			bGLFWInitialized = true;
		}

		// Apply styles before window creation!

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
		PF_CORE_ASSERT(m_NativeWindow, "Failed to create GLFW window!");

		glfwMakeContextCurrent(m_NativeWindow);
		glfwSetWindowUserPointer(m_NativeWindow, &WindowData);

		int Status = gladLoadGLLoader((GLADloadproc)glfwGetProcAddress);
		PF_CORE_ASSERT(Status, "Failed to initialize Glad!");

		SetVSync(true);

		// =========================================
		// GLFW Callbacks
		// =========================================

		// 1. Window Resize
		glfwSetWindowSizeCallback(m_NativeWindow, [](GLFWwindow* CurrentWindow, int Width, int Height)
		{
			FWindowData& Data = *(FWindowData*)glfwGetWindowUserPointer(CurrentWindow);
			Data.Width = Width;
			Data.Height = Height;

			WindowResizeEvent Event(Width, Height);
			Data.EventCallback(Event);
		});

		// 2. Window Close
		glfwSetWindowCloseCallback(m_NativeWindow, [](GLFWwindow* CurrentWindow)
		{
			FWindowData& Data = *(FWindowData*)glfwGetWindowUserPointer(CurrentWindow);

			WindowCloseEvent Event;
			Data.EventCallback(Event);
		});

		// 3. Window Focus
		glfwSetWindowFocusCallback(m_NativeWindow, [](GLFWwindow* CurrentWindow, int Focused)
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
		glfwSetWindowPosCallback(m_NativeWindow, [](GLFWwindow* CurrentWindow, int XPos, int YPos)
		{
			FWindowData& Data = *(FWindowData*)glfwGetWindowUserPointer(CurrentWindow);

		    WindowMovedEvent Event(XPos, YPos);
		    Data.EventCallback(Event);
		});

		// 5. Keyboard Events
		glfwSetKeyCallback(m_NativeWindow, [](GLFWwindow* CurrentWindow, int Key, int Scancode, int Action, int Mods)
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

		glfwSetCharCallback(m_NativeWindow, [](GLFWwindow* CurrentWindow, unsigned int KeyCode)
		{
			FWindowData& Data = *(FWindowData*)glfwGetWindowUserPointer(CurrentWindow);

			KeyTypedEvent Event(KeyCode);
			Data.EventCallback(Event);
		});

		// 6. Mouse Events
		glfwSetMouseButtonCallback(m_NativeWindow, [](GLFWwindow* CurrentWindow, int Button, int Action, int Mods)
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

		glfwSetScrollCallback(m_NativeWindow, [](GLFWwindow* CurrentWindow, double XOffset, double YOffset)
	    {
		    FWindowData& Data = *(FWindowData*)glfwGetWindowUserPointer(CurrentWindow);
	    
		    MouseScrolledEvent Event((float)XOffset, (float)YOffset);
		    Data.EventCallback(Event);
	    });

		glfwSetCursorPosCallback(m_NativeWindow, [](GLFWwindow* CurrentWindow, double XPos, double YPos)
		{
			FWindowData& Data = *(FWindowData*)glfwGetWindowUserPointer(CurrentWindow);

			MouseMovedEvent Event((float)XPos, (float)YPos);
			Data.EventCallback(Event);
		});
	}

	void WindowsWindow::Shutdown()
	{
		glfwDestroyWindow(m_NativeWindow);
	}

	void WindowsWindow::OnUpdate()
	{
		glfwPollEvents();
		glfwSwapBuffers(m_NativeWindow);
	}

	void WindowsWindow::SetVSync(bool bEnabled)
	{
		if (bEnabled)
		{
			glfwSwapInterval(1);
		}

		else
		{
			glfwSwapInterval(0);
		}

		WindowData.bVSync = bEnabled;
	}

	bool WindowsWindow::IsVSync() const
	{
		return WindowData.bVSync;
	}
}
