#pragma once

#include "Core/Application.h"

#include <filesystem>
#include <memory>
#include <optional>

namespace PulseForgeEditor
{
	[[nodiscard]] std::unique_ptr<PulseForge::Application> CreateEditorApplication(
		std::optional<std::filesystem::path> StartupProject);
}
