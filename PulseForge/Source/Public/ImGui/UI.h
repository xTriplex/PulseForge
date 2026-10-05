#pragma once

#include "Core/Core.h"

#include <string>

namespace PulseForge::UI
{
	PULSEFORGE_API void BeginWindow(const std::string& Title);
	PULSEFORGE_API void EndWindow();

	PULSEFORGE_API void Text(const std::string& Text);
	PULSEFORGE_API bool Button(const std::string& Label);

	PULSEFORGE_API void Separator();
	PULSEFORGE_API void SameLine();

	PULSEFORGE_API void ShowDemoWindow();
}
