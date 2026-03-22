#include "PulseForgeGame.h"
#include "Core/Log.h"
#include<iostream>

int main()
{
	PulseForge::Log::Init();

	PF_CORE_TRACE("Trace Message    - Check");
	PF_CORE_INFO("Info Message     - Check");
	PF_CORE_WARN("Warning Message  - Check");
	PF_CORE_ERROR("Error Message    - Check");
	PF_CORE_CRITICAL("Critical Message - Check");

	std::cout << "\n";

	PF_TRACE("Trace Message    - Check");
	PF_INFO("Info Message     - Check");
	PF_WARN("Warning Message  - Check");
	PF_ERROR("Error Message    - Check");
	PF_CRITICAL("Critical Message - Check");

	std::cout << "\n";
	std::cout << "Hello, PulseForge!\n";
}
