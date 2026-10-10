#pragma once

#include <span>
#include <string>
#include <string_view>

namespace PulseForgeEditor
{
	struct ConsoleEntryView
	{
		std::string_view Severity;
		std::string_view Logger;
		std::string_view Message;
	};

	inline void AppendConsoleEntry(std::string& Output, ConsoleEntryView Entry)
	{
		Output.push_back('[');
		Output.append(Entry.Severity.empty() ? std::string_view("unknown") : Entry.Severity);
		Output.push_back(']');
		if (!Entry.Logger.empty())
		{
			Output.append(" [");
			Output.append(Entry.Logger);
			Output.push_back(']');
		}
		if (!Entry.Message.empty())
		{
			Output.push_back(' ');
			Output.append(Entry.Message);
		}
	}

	[[nodiscard]] inline std::string FormatConsoleEntry(ConsoleEntryView Entry)
	{
		std::string Output;
		Output.reserve(Entry.Severity.size() + Entry.Logger.size() + Entry.Message.size() + 5);
		AppendConsoleEntry(Output, Entry);
		return Output;
	}

	[[nodiscard]] inline std::string FormatConsoleHistory(std::span<const ConsoleEntryView> Entries)
	{
		std::string Output;
		for (const ConsoleEntryView Entry : Entries)
		{
			if (!Output.empty())
				Output.push_back('\n');
			AppendConsoleEntry(Output, Entry);
		}
		return Output;
	}
}
