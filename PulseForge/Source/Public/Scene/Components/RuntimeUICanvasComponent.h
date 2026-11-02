#pragma once

#include "Assets/AssetID.h"
#include "UI/RuntimeUI.h"

namespace PulseForge
{
	struct RuntimeUICanvasComponent final
	{
		AssetID DocumentAsset;
		bool Enabled = true;
		RuntimeUIInputPolicy InputPolicy = RuntimeUIInputPolicy::Passive;
	};
}
