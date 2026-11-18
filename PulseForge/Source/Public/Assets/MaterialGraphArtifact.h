#pragma once

#include "Assets/MaterialGraphSource.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace PulseForge
{
	struct MaterialGraphCompiledArtifact final
	{
		struct Parameter final
		{
			UUID ID;
			std::string Name;
			float DefaultValue = 0.0f;
		};

		std::vector<std::byte> SpirV;
		std::string SemanticRecipe;
		std::string RecipeFingerprint;
		std::string Diagnostics;
		std::vector<UUID> SourceNodes;
		std::vector<Parameter> Parameters;
		uint64_t Generation = 0;
	};

	struct MaterialGraphCompileIdentity final
	{
		std::string RecipeFingerprint;
		std::vector<MaterialGraphCompiledArtifact::Parameter> Parameters;
	};
}
