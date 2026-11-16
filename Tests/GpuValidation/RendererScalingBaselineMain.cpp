#include "Core/Log.h"

#include <cstdlib>
#include <exception>

namespace PulseForge::GpuValidation
{
	int RunRendererScalingBaselineValidation(int ArgumentCount, char** Arguments);
}

int main(int ArgumentCount, char** Arguments)
{
	PulseForge::Log::Init();
	try
	{
		return PulseForge::GpuValidation::RunRendererScalingBaselineValidation(ArgumentCount, Arguments);
	}
	catch (const std::exception& Exception)
	{
		PF_CORE_CRITICAL("Renderer scaling baseline startup/runtime failure: {0}", Exception.what());
		return EXIT_FAILURE;
	}
}
