#pragma once

#include "Scene/Scene.h"

namespace PulseForge::GpuValidation
{
	void BuildGpuValidationSceneScenario(
		const Scene& Source, Scene& Destination, bool IncludeGeometry, bool IncludeEnvironment, bool IncludeDirectional);
} // namespace PulseForge::GpuValidation
