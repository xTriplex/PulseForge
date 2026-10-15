#pragma once

#include "EditorConsoleHistory.h"

#include <spdlog/common.h>

namespace PulseForgeEditor::Detail
{
	[[nodiscard]] EditorConsoleSeverity ConvertConsoleSeverity(spdlog::level::level_enum Level) noexcept;
}
