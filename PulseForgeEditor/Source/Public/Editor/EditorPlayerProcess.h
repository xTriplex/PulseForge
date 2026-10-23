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
		// True when more output was already waiting than this poll returned; remaining bytes stay queued.
		bool OutputTruncated = false;
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
		// Stops only the child owned by this process object. Safe to call repeatedly.
		void RequestStop() noexcept;

	private:
		explicit EditorPlayerProcess(std::unique_ptr<EditorPlayerProcessImpl> Implementation) noexcept;
		std::unique_ptr<EditorPlayerProcessImpl> m_Impl;
	};
} // namespace PulseForgeEditor
