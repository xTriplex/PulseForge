#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include "Editor/Detail/EditorPlayerProcessInternal.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace PulseForgeEditor
{
	namespace
	{
		std::expected<std::wstring, std::string> Utf8ToWide(std::string_view Text)
		{
			if (Text.empty()) return std::wstring{};
			if (Text.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
				return std::unexpected("A player argument exceeds the Windows text conversion limit");
			const int Length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Text.data(), static_cast<int>(Text.size()), nullptr, 0);
			if (Length <= 0) return std::unexpected("Player arguments must contain valid UTF-8");
			std::wstring Result(static_cast<size_t>(Length), L'\0');
			if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Text.data(), static_cast<int>(Text.size()), Result.data(), Length) !=
				Length)
				return std::unexpected("Could not convert player arguments from UTF-8");
			return Result;
		}

		std::wstring QuoteArgument(std::wstring_view Argument)
		{
			std::wstring Result;
			Result.push_back(L'"');
			size_t BackslashCount = 0;
			for (const wchar_t Character : Argument)
			{
				if (Character == L'\\')
				{
					++BackslashCount;
					continue;
				}
				if (Character == L'"')
				{
					Result.append(BackslashCount * 2 + 1, L'\\');
					Result.push_back(L'"');
				}
				else
				{
					Result.append(BackslashCount, L'\\');
					Result.push_back(Character);
				}
				BackslashCount = 0;
			}
			Result.append(BackslashCount * 2, L'\\');
			Result.push_back(L'"');
			return Result;
		}

		std::expected<std::wstring, std::string> MakeCommandLine(const EditorPlayerLaunchSpec& Spec)
		{
			std::wstring CommandLine = QuoteArgument(Spec.Executable.native());
			for (const std::string& Argument : Spec.Arguments)
			{
				auto WideArgument = Utf8ToWide(Argument);
				if (!WideArgument) return std::unexpected(WideArgument.error());
				CommandLine.push_back(L' ');
				CommandLine += QuoteArgument(*WideArgument);
			}
			if (CommandLine.size() >= 32767) return std::unexpected("The player command line exceeds the Windows process limit");
			return CommandLine;
		}

		class WindowsEditorPlayerProcess final : public EditorPlayerProcessImpl
		{
		public:
			WindowsEditorPlayerProcess(HANDLE Process, HANDLE Output) noexcept : m_Process(Process), m_Output(Output) {}

			~WindowsEditorPlayerProcess() override
			{
				RequestStop();
				if (m_Output && m_Output != INVALID_HANDLE_VALUE) CloseHandle(m_Output);
				if (m_Process && m_Process != INVALID_HANDLE_VALUE) CloseHandle(m_Process);
			}

			EditorPlayerProcessUpdate Poll(size_t MaximumOutputBytes) override
			{
				EditorPlayerProcessUpdate Update;
				ReadOutput(MaximumOutputBytes, Update);
				if (m_Terminal)
				{
					Update.Running = false;
					Update.ExitCode = m_ExitCode;
					return Update;
				}

				const DWORD WaitResult = WaitForSingleObject(m_Process, 0);
				if (WaitResult == WAIT_TIMEOUT)
				{
					Update.Running = true;
					return Update;
				}
				if (WaitResult == WAIT_FAILED)
				{
					Update.Running = true;
					Update.Error = "Could not inspect player process state (Windows error " + std::to_string(GetLastError()) + ")";
					return Update;
				}

				DWORD ExitCode = 0;
				if (!GetExitCodeProcess(m_Process, &ExitCode))
				{
					Update.Error = "Could not read player exit code (Windows error " + std::to_string(GetLastError()) + ")";
					Update.Running = false;
					m_Terminal = true;
					return Update;
				}
				m_ExitCode = static_cast<int64_t>(ExitCode);
				m_Terminal = true;
				Update.Running = false;
				Update.ExitCode = m_ExitCode;
				return Update;
			}

			void RequestStop() noexcept override
			{
				if (m_Terminal || m_StopRequested || !m_Process || m_Process == INVALID_HANDLE_VALUE) return;
				if (TerminateProcess(m_Process, 1)) m_StopRequested = true;
			}

		private:
			void ReadOutput(size_t MaximumOutputBytes, EditorPlayerProcessUpdate& Update)
			{
				if (!m_Output || m_Output == INVALID_HANDLE_VALUE) return;
				DWORD Available = 0;
				if (!PeekNamedPipe(m_Output, nullptr, 0, nullptr, &Available, nullptr))
				{
					const DWORD Error = GetLastError();
					if (Error != ERROR_BROKEN_PIPE)
						Update.Error = "Could not inspect player output (Windows error " + std::to_string(Error) + ")";
					CloseHandle(m_Output);
					m_Output = INVALID_HANDLE_VALUE;
					return;
				}
				if (Available == 0 || MaximumOutputBytes == 0) return;

				const DWORD ReadLimit = static_cast<DWORD>((std::min)(MaximumOutputBytes, static_cast<size_t>(Available)));
				Update.OutputChunk.resize(ReadLimit);
				DWORD Read = 0;
				if (!ReadFile(m_Output, Update.OutputChunk.data(), ReadLimit, &Read, nullptr))
				{
					const DWORD Error = GetLastError();
					if (Error != ERROR_BROKEN_PIPE)
						Update.Error = "Could not read player output (Windows error " + std::to_string(Error) + ")";
					Update.OutputChunk.clear();
					if (Error == ERROR_BROKEN_PIPE)
					{
						CloseHandle(m_Output);
						m_Output = INVALID_HANDLE_VALUE;
					}
					return;
				}
				Update.OutputChunk.resize(Read);
				Update.OutputTruncated = Available > Read;
			}

			HANDLE m_Process = nullptr;
			HANDLE m_Output = nullptr;
			std::optional<int64_t> m_ExitCode;
			bool m_Terminal = false;
			bool m_StopRequested = false;
		};
	} // namespace

	std::expected<std::unique_ptr<EditorPlayerProcessImpl>, std::string> CreatePlatformEditorPlayerProcess(
		const EditorPlayerLaunchSpec& Spec)
	{
		auto CommandLine = MakeCommandLine(Spec);
		if (!CommandLine) return std::unexpected(CommandLine.error());
		std::vector<wchar_t> MutableCommandLine(CommandLine->begin(), CommandLine->end());
		MutableCommandLine.push_back(L'\0');
		const std::wstring WorkingDirectory = Spec.WorkingDirectory.native();

		SECURITY_ATTRIBUTES PipeSecurity{ .nLength = sizeof(SECURITY_ATTRIBUTES), .lpSecurityDescriptor = nullptr, .bInheritHandle = TRUE };
		HANDLE ReadPipe = nullptr;
		HANDLE WritePipe = nullptr;
		if (!CreatePipe(&ReadPipe, &WritePipe, &PipeSecurity, 0))
			return std::unexpected("Could not create player output pipe (Windows error " + std::to_string(GetLastError()) + ")");
		if (!SetHandleInformation(ReadPipe, HANDLE_FLAG_INHERIT, 0))
		{
			const DWORD Error = GetLastError();
			CloseHandle(ReadPipe);
			CloseHandle(WritePipe);
			return std::unexpected("Could not isolate the player output pipe (Windows error " + std::to_string(Error) + ")");
		}

		HANDLE NullInput = CreateFileW(
			L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &PipeSecurity, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (NullInput == INVALID_HANDLE_VALUE)
		{
			const DWORD Error = GetLastError();
			CloseHandle(ReadPipe);
			CloseHandle(WritePipe);
			return std::unexpected("Could not open null player input (Windows error " + std::to_string(Error) + ")");
		}

		SIZE_T AttributeListSize = 0;
		(void)InitializeProcThreadAttributeList(nullptr, 1, 0, &AttributeListSize);
		std::unique_ptr<std::byte[]> AttributeStorage(new (std::nothrow) std::byte[AttributeListSize]);
		if (!AttributeStorage)
		{
			CloseHandle(NullInput);
			CloseHandle(ReadPipe);
			CloseHandle(WritePipe);
			return std::unexpected("Could not allocate player process attributes");
		}
		auto* AttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(AttributeStorage.get());
		if (!InitializeProcThreadAttributeList(AttributeList, 1, 0, &AttributeListSize))
		{
			const DWORD Error = GetLastError();
			CloseHandle(NullInput);
			CloseHandle(ReadPipe);
			CloseHandle(WritePipe);
			return std::unexpected("Could not initialize player process attributes (Windows error " + std::to_string(Error) + ")");
		}

		HANDLE InheritedHandles[] = { WritePipe, NullInput };
		const bool HandlesAdded = UpdateProcThreadAttribute(AttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, InheritedHandles,
									  sizeof(InheritedHandles), nullptr, nullptr) != FALSE;
		if (!HandlesAdded)
		{
			const DWORD Error = GetLastError();
			DeleteProcThreadAttributeList(AttributeList);
			CloseHandle(NullInput);
			CloseHandle(ReadPipe);
			CloseHandle(WritePipe);
			return std::unexpected("Could not restrict inherited player handles (Windows error " + std::to_string(Error) + ")");
		}

		STARTUPINFOEXW Startup{};
		Startup.StartupInfo.cb = sizeof(Startup);
		Startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
		Startup.StartupInfo.hStdInput = NullInput;
		Startup.StartupInfo.hStdOutput = WritePipe;
		Startup.StartupInfo.hStdError = WritePipe;
		Startup.lpAttributeList = AttributeList;
		PROCESS_INFORMATION ProcessInfo{};
		const BOOL Started = CreateProcessW(Spec.Executable.c_str(), MutableCommandLine.data(), nullptr, nullptr, TRUE,
			EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr, WorkingDirectory.c_str(), &Startup.StartupInfo, &ProcessInfo);
		const DWORD StartError = Started ? ERROR_SUCCESS : GetLastError();
		DeleteProcThreadAttributeList(AttributeList);
		CloseHandle(NullInput);
		CloseHandle(WritePipe);
		if (!Started)
		{
			CloseHandle(ReadPipe);
			return std::unexpected("Could not launch player process (Windows error " + std::to_string(StartError) + ")");
		}

		CloseHandle(ProcessInfo.hThread);
		auto* RawImplementation = new (std::nothrow) WindowsEditorPlayerProcess(ProcessInfo.hProcess, ReadPipe);
		if (!RawImplementation)
		{
			(void)TerminateProcess(ProcessInfo.hProcess, 1);
			CloseHandle(ProcessInfo.hProcess);
			CloseHandle(ReadPipe);
			return std::unexpected("Could not allocate player process state");
		}
		return std::unique_ptr<EditorPlayerProcessImpl>(RawImplementation);
	}
} // namespace PulseForgeEditor
