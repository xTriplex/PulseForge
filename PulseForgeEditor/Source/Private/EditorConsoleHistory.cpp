#include "EditorConsoleHistory.h"

#include "Core/Log.h"
#include "EditorConsoleSeverity.h"

#include <algorithm>
#include <utility>

namespace PulseForgeEditor
{
	void EditorConsoleHistory::Append(std::string_view Logger, EditorConsoleSeverity Severity, std::string_view Text)
	{
		const size_t MessageSize = (std::min)(Text.size(), MaxMessageBytes);
		std::string SafeText = MessageSize == 0 ? std::string{} : std::string(Text.data(), MessageSize);
		if (Text.size() > MaxMessageBytes) SafeText += " [truncated]";
		std::replace(SafeText.begin(), SafeText.end(), '\r', ' ');
		std::replace(SafeText.begin(), SafeText.end(), '\n', ' ');

		std::lock_guard<std::mutex> Lock(m_Mutex);
		m_Messages.push_back({ ++m_NextSequence, std::string(Logger), Severity, std::move(SafeText) });
		if (m_Messages.size() > Capacity) m_Messages.pop_front();
		++m_Revision;
	}

	void EditorConsoleHistory::Append(const PulseForge::LogEntry& Entry)
	{
		Append(Entry.Logger, Detail::ConvertConsoleSeverity(Entry.Level), Entry.Message);
	}

	void EditorConsoleHistory::AppendRecent(const std::vector<PulseForge::LogEntry>& Entries)
	{
		for (const PulseForge::LogEntry& Entry : Entries)
			Append(Entry);
	}

	bool EditorConsoleHistory::CopyMessagesIfChanged(
		uint64_t PreviousRevision, std::vector<EditorConsoleMessage>& Destination, uint64_t& CurrentRevision) const
	{
		std::lock_guard<std::mutex> Lock(m_Mutex);
		CurrentRevision = m_Revision;
		if (CurrentRevision == PreviousRevision) return false;

		Destination.assign(m_Messages.begin(), m_Messages.end());
		return true;
	}

	void EditorConsoleHistory::Clear()
	{
		std::lock_guard<std::mutex> Lock(m_Mutex);
		m_Messages.clear();
		++m_Revision;
	}
} // namespace PulseForgeEditor
