#include "Editor/EditorConsoleSeverity.h"

namespace PulseForgeEditor::Detail
{
	EditorConsoleSeverity ConvertConsoleSeverity(PulseForge::LogLevel Level) noexcept
	{
		switch (Level)
		{
		case PulseForge::LogLevel::Trace:
			return EditorConsoleSeverity::Trace;
		case PulseForge::LogLevel::Debug:
			return EditorConsoleSeverity::Debug;
		case PulseForge::LogLevel::Info:
			return EditorConsoleSeverity::Info;
		case PulseForge::LogLevel::Warning:
			return EditorConsoleSeverity::Warning;
		case PulseForge::LogLevel::Error:
			return EditorConsoleSeverity::Error;
		case PulseForge::LogLevel::Critical:
			return EditorConsoleSeverity::Critical;
		case PulseForge::LogLevel::Unknown:
		default:
			return EditorConsoleSeverity::Unknown;
		}
	}

	const char* ConsoleSeverityLabel(EditorConsoleSeverity Severity) noexcept
	{
		switch (Severity)
		{
		case EditorConsoleSeverity::Trace:
			return "TRACE";
		case EditorConsoleSeverity::Debug:
			return "DEBUG";
		case EditorConsoleSeverity::Info:
			return "INFO";
		case EditorConsoleSeverity::Warning:
			return "WARNING";
		case EditorConsoleSeverity::Error:
			return "ERROR";
		case EditorConsoleSeverity::Critical:
			return "CRITICAL";
		case EditorConsoleSeverity::Unknown:
		default:
			return "UNKNOWN";
		}
	}
} // namespace PulseForgeEditor::Detail
