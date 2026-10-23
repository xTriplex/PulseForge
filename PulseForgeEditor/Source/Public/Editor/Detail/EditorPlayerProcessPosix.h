#pragma once

#include "Editor/Detail/EditorPlayerProcessInternal.h"

#include <cerrno>
#include <cstring>
#include <spawn.h>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <new>

extern char** environ;

namespace PulseForgeEditor::Detail
{
	class PosixEditorPlayerProcess final : public EditorPlayerProcessImpl
	{
	public:
		PosixEditorPlayerProcess(pid_t ProcessID, int OutputDescriptor) noexcept
			: m_ProcessID(ProcessID), m_OutputDescriptor(OutputDescriptor)
		{
		}

		~PosixEditorPlayerProcess() override
		{
			RequestStop();
			if (!m_Reaped && m_ProcessID > 0)
			{
				(void)::kill(m_ProcessID, SIGKILL);
				int Status = 0;
				while (::waitpid(m_ProcessID, &Status, 0) < 0 && errno == EINTR)
				{
				}
			}
			CloseOutputDescriptor();
		}

		EditorPlayerProcessUpdate Poll(size_t MaximumOutputBytes) override
		{
			EditorPlayerProcessUpdate Update;
			ReadOutput(MaximumOutputBytes, Update);
			ObserveExit(Update);
			return Update;
		}

		void RequestStop() noexcept override
		{
			if (m_Reaped || m_StopRequested || m_ProcessID <= 0) return;
			int WaitError = 0;
			if (ObserveExitStatus(WaitError)) return;
			if (::kill(m_ProcessID, SIGTERM) == 0 || errno == ESRCH) m_StopRequested = true;
		}

	private:
		void ReadOutput(size_t MaximumOutputBytes, EditorPlayerProcessUpdate& Update)
		{
			if (m_OutputDescriptor < 0) return;

			if (MaximumOutputBytes > 0)
			{
				std::vector<char> Buffer(MaximumOutputBytes);
				for (;;)
				{
					const ssize_t Read = ::read(m_OutputDescriptor, Buffer.data(), Buffer.size());
					if (Read > 0)
					{
						Update.OutputChunk.assign(Buffer.data(), static_cast<size_t>(Read));
						break;
					}
					if (Read == 0)
					{
						CloseOutputDescriptor();
						break;
					}
					if (errno == EINTR) continue;
					if (errno != EAGAIN && errno != EWOULDBLOCK)
						Update.Error = "Could not read player output: " + std::string(std::strerror(errno));
					break;
				}
			}

			if (m_OutputDescriptor >= 0)
			{
				pollfd Descriptor{ .fd = m_OutputDescriptor, .events = POLLIN };
				const int Result = ::poll(&Descriptor, 1, 0);
				if (Result > 0 && (Descriptor.revents & POLLIN) != 0) Update.OutputTruncated = true;
				else if (Result < 0 && errno != EINTR)
					Update.Error = "Could not inspect player output: " + std::string(std::strerror(errno));
			}
		}

		void ObserveExit(EditorPlayerProcessUpdate& Update)
		{
			if (m_Reaped)
			{
				Update.Running = false;
				if (m_HasExitCode) Update.ExitCode = m_ExitCode;
				if (m_WaitErrorCode != 0)
					Update.Error = "Could not inspect player process state: " + std::string(std::strerror(m_WaitErrorCode));
				return;
			}

			int WaitError = 0;
			if (!ObserveExitStatus(WaitError))
			{
				Update.Running = true;
				if (WaitError != 0) Update.Error = "Could not inspect player process state: " + std::string(std::strerror(WaitError));
				return;
			}
			Update.Running = false;
			if (m_HasExitCode) Update.ExitCode = m_ExitCode;
			if (WaitError != 0) Update.Error = "Could not inspect player process state: " + std::string(std::strerror(WaitError));
		}

		bool ObserveExitStatus(int& ErrorCode) noexcept
		{
			ErrorCode = m_WaitErrorCode;
			if (m_Reaped) return true;
			int Status = 0;
			pid_t Result = 0;
			do
			{
				Result = ::waitpid(m_ProcessID, &Status, WNOHANG);
			} while (Result < 0 && errno == EINTR);
			if (Result == 0)
			{
				ErrorCode = 0;
				m_WaitErrorCode = 0;
				return false;
			}
			if (Result < 0)
			{
				ErrorCode = errno;
				m_WaitErrorCode = ErrorCode;
				if (ErrorCode == ECHILD) m_Reaped = true;
				return m_Reaped;
			}
			m_Reaped = true;
			m_HasExitCode = true;
			if (WIFEXITED(Status)) m_ExitCode = static_cast<int64_t>(WEXITSTATUS(Status));
			else if (WIFSIGNALED(Status)) m_ExitCode = static_cast<int64_t>(128 + WTERMSIG(Status));
			return true;
		}

		void CloseOutputDescriptor() noexcept
		{
			if (m_OutputDescriptor < 0) return;
			(void)::close(m_OutputDescriptor);
			m_OutputDescriptor = -1;
		}

		pid_t m_ProcessID = -1;
		int m_OutputDescriptor = -1;
		int64_t m_ExitCode = 0;
		bool m_Reaped = false;
		bool m_HasExitCode = false;
		bool m_StopRequested = false;
		int m_WaitErrorCode = 0;
	};

	inline std::expected<std::unique_ptr<EditorPlayerProcessImpl>, std::string> CreatePosixEditorPlayerProcess(
		const EditorPlayerLaunchSpec& Spec)
	{
		int PipeDescriptors[2] = { -1, -1 };
		if (::pipe(PipeDescriptors) != 0)
			return std::unexpected("Could not create player output pipe: " + std::string(std::strerror(errno)));

		const auto Close = [](int& Descriptor)
		{
			if (Descriptor < 0) return;
			(void)::close(Descriptor);
			Descriptor = -1;
		};
		for (int& Descriptor : PipeDescriptors)
		{
			if (Descriptor > STDERR_FILENO) continue;
			const int Replacement = ::fcntl(Descriptor, F_DUPFD, STDERR_FILENO + 1);
			if (Replacement < 0)
			{
				const std::string Error = "Could not isolate player output descriptors: " + std::string(std::strerror(errno));
				Close(PipeDescriptors[0]);
				Close(PipeDescriptors[1]);
				return std::unexpected(Error);
			}
			Close(Descriptor);
			Descriptor = Replacement;
		}
		for (const int Descriptor : PipeDescriptors)
		{
			const int Flags = ::fcntl(Descriptor, F_GETFD);
			if (Flags < 0 || ::fcntl(Descriptor, F_SETFD, Flags | FD_CLOEXEC) < 0)
			{
				const std::string Error = "Could not mark player output pipe close-on-exec: " + std::string(std::strerror(errno));
				Close(PipeDescriptors[0]);
				Close(PipeDescriptors[1]);
				return std::unexpected(Error);
			}
		}
		const int ReadFlags = ::fcntl(PipeDescriptors[0], F_GETFL);
		if (ReadFlags < 0 || ::fcntl(PipeDescriptors[0], F_SETFL, ReadFlags | O_NONBLOCK) < 0)
		{
			const std::string Error = "Could not configure nonblocking player output: " + std::string(std::strerror(errno));
			Close(PipeDescriptors[0]);
			Close(PipeDescriptors[1]);
			return std::unexpected(Error);
		}

		posix_spawn_file_actions_t Actions;
		int ErrorCode = ::posix_spawn_file_actions_init(&Actions);
		if (ErrorCode != 0)
		{
			Close(PipeDescriptors[0]);
			Close(PipeDescriptors[1]);
			return std::unexpected("Could not initialize player process file actions: " + std::string(std::strerror(ErrorCode)));
		}
		const auto AddAction = [&ErrorCode](int Result)
		{
			if (ErrorCode == 0 && Result != 0) ErrorCode = Result;
		};
		AddAction(::posix_spawn_file_actions_adddup2(&Actions, PipeDescriptors[1], STDOUT_FILENO));
		AddAction(::posix_spawn_file_actions_adddup2(&Actions, PipeDescriptors[1], STDERR_FILENO));
		AddAction(::posix_spawn_file_actions_addopen(&Actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0));
		AddAction(::posix_spawn_file_actions_addclose(&Actions, PipeDescriptors[0]));
		AddAction(::posix_spawn_file_actions_addclose(&Actions, PipeDescriptors[1]));
		if (ErrorCode == 0) AddAction(::posix_spawn_file_actions_addchdir_np(&Actions, Spec.WorkingDirectory.c_str()));
		if (ErrorCode != 0)
		{
			(void)::posix_spawn_file_actions_destroy(&Actions);
			Close(PipeDescriptors[0]);
			Close(PipeDescriptors[1]);
			return std::unexpected("Could not configure player process file actions: " + std::string(std::strerror(ErrorCode)));
		}

		std::vector<std::string> ArgumentStorage;
		ArgumentStorage.reserve(Spec.Arguments.size() + 1);
		ArgumentStorage.push_back(Spec.Executable.native());
		ArgumentStorage.insert(ArgumentStorage.end(), Spec.Arguments.begin(), Spec.Arguments.end());
		std::vector<char*> Arguments;
		Arguments.reserve(ArgumentStorage.size() + 1);
		for (std::string& Argument : ArgumentStorage)
			Arguments.push_back(Argument.data());
		Arguments.push_back(nullptr);

		pid_t ProcessID = -1;
		ErrorCode = ::posix_spawn(&ProcessID, Spec.Executable.c_str(), &Actions, nullptr, Arguments.data(), environ);
		(void)::posix_spawn_file_actions_destroy(&Actions);
		Close(PipeDescriptors[1]);
		if (ErrorCode != 0)
		{
			Close(PipeDescriptors[0]);
			return std::unexpected("Could not launch player process: " + std::string(std::strerror(ErrorCode)));
		}

		auto* RawImplementation = new (std::nothrow) PosixEditorPlayerProcess(ProcessID, PipeDescriptors[0]);
		if (!RawImplementation)
		{
			(void)::kill(ProcessID, SIGKILL);
			int Status = 0;
			while (::waitpid(ProcessID, &Status, 0) < 0 && errno == EINTR)
			{
			}
			Close(PipeDescriptors[0]);
			return std::unexpected("Could not allocate player process state");
		}
		return std::unique_ptr<EditorPlayerProcessImpl>(RawImplementation);
	}
} // namespace PulseForgeEditor::Detail
