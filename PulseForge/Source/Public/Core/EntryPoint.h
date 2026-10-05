#pragma once

#include "Core/Core.h"
#include "Core/Log.h"
#include "Core/Application.h"

#include <cstdlib>
#include <exception>
#include <stdexcept>

#ifdef PF_PLATFORM_WINDOWS

extern std::unique_ptr<PulseForge::Application> PulseForge::CreateApplication();

int main(int argc, char** argv)
{
	PulseForge::Log::Init();
	PF_CORE_INFO("Engine Logger Initialized!");

	try
	{
		auto App = PulseForge::CreateApplication();
		if (!App)
			throw std::runtime_error("Client application factory returned null");

		PF_INFO("Client Application Created!");
		App->Run();
		return EXIT_SUCCESS;
	}
	catch (const std::exception& Exception)
	{
		PF_CORE_CRITICAL("Application startup/runtime failure: {0}", Exception.what());
		return EXIT_FAILURE;
	}
}

#endif
