#pragma once

#include "Core/Application.h"
#include "Editor/EditorCommandLine.h"

#include <filesystem>
#include <memory>
#include <optional>

namespace PulseForgeEditor
{
	[[nodiscard]] std::unique_ptr<PulseForge::Application> CreateEditorApplication(EditorCommandLineOptions Options = {});
}
