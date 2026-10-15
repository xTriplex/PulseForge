#include "EditorConsoleSeverity.h"

namespace PulseForgeEditor::Detail
{
	EditorConsoleSeverity ConvertConsoleSeverity(spdlog::level::level_enum Level) noexcept
	{
		switch (Level)
		{
		case spdlog::level::trace:
			return EditorConsoleSeverity::Trace;
		case spdlog::level::debug:
			return EditorConsoleSeverity::Debug;
		case spdlog::level::info:
			return EditorConsoleSeverity::Info;
		case spdlog::level::warn:
			return EditorConsoleSeverity::Warning;
		case spdlog::level::err:
			return EditorConsoleSeverity::Error;
		case spdlog::level::critical:
			return EditorConsoleSeverity::Critical;
		case spdlog::level::off:
		case spdlog::level::n_levels:
		default:
			return EditorConsoleSeverity::Unknown;
		}
	}
} // namespace PulseForgeEditor::Detail
