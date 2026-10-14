#pragma once

#include "Renderer/Texture.h"

namespace PulseForge
{
	struct EnvironmentLightingTextures
	{
		TextureHandle Environment;
		TextureHandle DiffuseIrradiance;
		TextureHandle PrefilteredSpecular;
		TextureHandle BrdfIntegrationLut;
	};
}
