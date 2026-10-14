#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace PulseForgeEditor
{
	enum class EditorConsoleSeverity : uint8_t
	{
		Unknown,
		Trace,
		Debug,
		Info,
		Warning,
		Error,
		Critical
	};

	struct EditorConsoleMessage
	{
		uint64_t Sequence = 0;
		std::string Logger;
		EditorConsoleSeverity Severity = EditorConsoleSeverity::Info;
		std::string Text;
	};

	class EditorConsoleHistory final
	{
	public:
		static constexpr size_t Capacity = 2000;
		static constexpr size_t MaxMessageBytes = 8192;

		void Append(std::string_view Logger, EditorConsoleSeverity Severity, std::string_view Text);
		[[nodiscard]] bool CopyMessagesIfChanged(
			uint64_t PreviousRevision,
			std::vector<EditorConsoleMessage>& Destination,
			uint64_t& CurrentRevision) const;
		void Clear();

	private:
		mutable std::mutex m_Mutex;
		std::deque<EditorConsoleMessage> m_Messages;
		uint64_t m_Revision = 0;
		uint64_t m_NextSequence = 0;
	};
}
