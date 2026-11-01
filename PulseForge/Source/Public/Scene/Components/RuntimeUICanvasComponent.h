#pragma once

#include "Assets/AssetID.h"

namespace PulseForge
{
	struct RuntimeUICanvasComponent final
	{
		AssetID DocumentAsset;
		bool Enabled = true;
	};
}
