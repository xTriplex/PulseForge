#include "PulseForgeGame.h"
#include "Core/Application.h"
#include "Core/EntryPoint.h"
#include "Core/Log.h"

class PulseForgeGameApp : public PulseForge::Application
{
public:
	PulseForgeGameApp()
	{
		PF_INFO("Hello, PulseForge!");
	}

	~PulseForgeGameApp()
	{

	}
};

PulseForge::Application* PulseForge::CreateApplication()
{
	return new PulseForgeGameApp();
}
