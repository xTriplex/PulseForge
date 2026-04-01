#include "PulseForgeGame.h"
#include "Core/Application.h"
#include "Core/EntryPoint.h"
#include "Core/Log.h"

class ExampleLayer : public PulseForge::Layer
{
public:
	ExampleLayer()
		: Layer("Example")
	{

	}

	void OnUpdate() override
	{
		PF_INFO("ExampleLayer::Update");
	}

	void OnEvent(PulseForge::Event& Event) override
	{
		PF_TRACE("{0}", Event.ToString());
	}
};

class PulseForgeGameApp : public PulseForge::Application
{
public:
	PulseForgeGameApp()
	{
		PF_INFO("Hello, PulseForge!");

		PushLayer(new ExampleLayer());
	}

	~PulseForgeGameApp()
	{

	}
};

PulseForge::Application* PulseForge::CreateApplication()
{
	return new PulseForgeGameApp();
}
