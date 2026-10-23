#pragma once

#include "Client/PlayerLaunchConfiguration.h"
#include "Core/Application.h"

class PulseForgeGameApp final : public PulseForge::Application
{
public:
	explicit PulseForgeGameApp(PulseForgeGame::PlayerLaunchConfiguration Configuration, bool EditorControlEnabled = false);
};
