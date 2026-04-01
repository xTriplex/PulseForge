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
			for (Layer* Layer : m_LayerStack)
			{
				Layer->OnUpdate();
			}

			m_Window->OnUpdate();
		}
	}

	void Application::OnEvent(Event& E)
	{
		PF_CORE_INFO("{0}", E.ToString());

		EventDispatcher Dispatcher(E);

		Dispatcher.Dispatch<WindowCloseEvent>(std::bind(&Application::OnWindowClose, this, std::placeholders::_1));

		for (auto It = m_LayerStack.rbegin(); It != m_LayerStack.rend(); ++It)
		{
			(*It)->OnEvent(E);
			if (E.bHandled)
			{
				break;
			}
		}
	}

	void Application::PushLayer(Layer* Layer)
	{
		m_LayerStack.PushLayer(Layer);
	}

	void Application::PushOverlay(Layer* Overlay)
	{
		m_LayerStack.PushOverlay(Overlay);
	}

	bool Application::OnWindowClose(WindowCloseEvent& E)
	{
		m_Running = false;
		return true;
	}
}
