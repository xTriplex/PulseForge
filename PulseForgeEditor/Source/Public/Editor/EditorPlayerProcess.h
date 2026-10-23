#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace PulseForgeEditor
{
	struct EditorPlayerLaunchSpec
	{
		std::filesystem::path Executable;
		std::filesystem::path WorkingDirectory;
		// Arguments after argv[0]. Text arguments are encoded as UTF-8 on every platform.
		std::vector<std::string> Arguments;
	};

	struct EditorPlayerProcessUpdate
	{
		bool Running = false;
		std::optional<int64_t> ExitCode;
		std::string OutputChunk;
		// True when the child exited after a stop request without requiring forced termination.
		bool CooperativeStopObserved = false;
		bool ForcedTermination = false;
		// OutputComplete means EOF was observed or collection stopped after an explicit terminal read error or drain deadline.
		bool OutputComplete = false;
		// True when more output was waiting than this bounded poll returned, or unread output was abandoned.
		bool OutputTruncated = false;
		bool OutputReadFailed = false;
		std::optional<std::string> Error;
	};

	class EditorPlayerProcessImpl;

	class EditorPlayerProcess final
	{
	public:
		[[nodiscard]] static std::expected<std::unique_ptr<EditorPlayerProcess>, std::string> Launch(const EditorPlayerLaunchSpec& Spec);

		EditorPlayerProcess(const EditorPlayerProcess&) = delete;
		EditorPlayerProcess& operator=(const EditorPlayerProcess&) = delete;
		EditorPlayerProcess(EditorPlayerProcess&&) = delete;
		EditorPlayerProcess& operator=(EditorPlayerProcess&&) = delete;
		~EditorPlayerProcess();

		// Nonblocking; bounds the amount of process output returned in one update.
		[[nodiscard]] EditorPlayerProcessUpdate Poll(size_t MaximumOutputBytes = 64 * 1024);
		// Requests cooperative shutdown of only the owned child. A bounded platform fallback follows if needed.
		void RequestStop() noexcept;

	private:
		explicit EditorPlayerProcess(std::unique_ptr<EditorPlayerProcessImpl> Implementation) noexcept;
		std::unique_ptr<EditorPlayerProcessImpl> m_Impl;
		bool m_StopRequested = false;
		bool m_CooperativeControlAcknowledged = false;
		std::string m_ControlAcknowledgementTail;
	};
} // namespace PulseForgeEditor
