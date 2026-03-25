#pragma once

#include "Core/Core.h"
#include "Core/Log.h"
#include "Core/Application.h"

#ifdef PF_PLATFORM_WINDOWS

extern PulseForge::Application* PulseForge::CreateApplication();

int main(int argc, char** argv)
{
	PulseForge::Log::Init();
	PF_CORE_INFO("Engine Logger Initialized!");

	auto App = PulseForge::CreateApplication();

	PF_INFO("Client Application Created!");

	App->Run();

	delete App;
}

#endif
