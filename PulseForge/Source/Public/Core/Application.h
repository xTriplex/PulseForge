#pragma once

#include "Core/Core.h"
#include "Window/Window.h"
#include "Core/LayerStack.h"
#include "Events/ApplicationEvent.h"

#include <memory>

namespace PulseForge
{
	class PULSEFORGE_API Application
	{
	public:
		Application();
		virtual ~Application();

		void Run();
		void OnEvent(Event& E);
		void PushLayer(Layer* Layer);
		void PushOverlay(Layer* Overlay);

	private:
		bool OnWindowClose(WindowCloseEvent& E);

	private:
		std::unique_ptr<Window> m_Window;
		bool m_Running = true;

		LayerStack m_LayerStack;
	};

	// To be defined in the Client
	Application* CreateApplication();
}
