#pragma once

#include "EditorConsoleHistory.h"
#include "Core/Log.h"

namespace PulseForgeEditor::Detail
{
	[[nodiscard]] EditorConsoleSeverity ConvertConsoleSeverity(PulseForge::LogLevel Level) noexcept;
}
