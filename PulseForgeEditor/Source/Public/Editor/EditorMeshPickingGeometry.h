#pragma once

#include "Assets/AssetID.h"

#include <cstdint>
#include <expected>
#include <functional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace PulseForgeEditor
{
	struct EditorMeshPickingGeometry
	{
		std::vector<glm::vec3> Positions;
		std::vector<uint32_t> Indices;
	};

	using EditorMeshGeometryResolver =
		std::function<std::expected<const EditorMeshPickingGeometry*, std::string>(const PulseForge::AssetID&)>;
} // namespace PulseForgeEditor
