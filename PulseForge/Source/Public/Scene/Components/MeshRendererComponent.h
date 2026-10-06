#pragma once

#include "Assets/AssetID.h"

#include <optional>

namespace PulseForge
{
	struct MeshRendererComponent
	{
		AssetID MeshAsset;
		std::optional<AssetID> MaterialAsset;
	};
}
