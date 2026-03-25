#include "pfpch.h"
#include "Core/Application.h"

namespace PulseForge
{
	Application::Application()
	{
		m_Window = std::unique_ptr<Window>(Window::Create());
		m_Window->SetEventCallback(std::bind(&Application::OnEvent, this, std::placeholders::_1));
	}

	Application::~Application()
	{

	}

	void Application::Run()
	{
		while (m_Running)
		{
			m_Window->OnUpdate();
		}
	}

	void Application::OnEvent(Event& E)
	{
		PF_CORE_INFO("{0}", E.ToString());

		EventDispatcher Dispatcher(E);

		Dispatcher.Dispatch<WindowCloseEvent>(std::bind(&Application::OnWindowClose, this, std::placeholders::_1));
	}

	bool Application::OnWindowClose(WindowCloseEvent& E)
	{
		m_Running = false;
		return true;
	}
}
