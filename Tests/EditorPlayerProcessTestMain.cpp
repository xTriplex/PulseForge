#include "Editor/EditorPlayerProcess.h"
#include "Editor/EditorPlayerExecutablePath.h"
#include "TestSupport.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace
{
	using namespace std::chrono_literals;

	constexpr int64_t SuccessfulChildExitCode = 37;
	constexpr std::string_view ChildArgumentUtf8 = "argument with spaces \xCE\xA9 \xF0\x9F\x9A\x80";

	std::string PathToUtf8(const std::filesystem::path& Path)
	{
		const std::u8string Encoded = Path.u8string();
		return { reinterpret_cast<const char*>(Encoded.data()), Encoded.size() };
	}

	std::filesystem::path PathFromUtf8(std::string_view Value)
	{
		std::u8string Encoded;
		Encoded.reserve(Value.size());
		for (const char Byte : Value) Encoded.push_back(static_cast<char8_t>(static_cast<unsigned char>(Byte)));
		return std::filesystem::path(Encoded);
	}

	std::string NormalizeNewlines(std::string Value)
	{
		for (size_t Position = 0; (Position = Value.find("\r\n", Position)) != std::string::npos;)
			Value.erase(Position, 1);
		return Value;
	}

	void WriteChildOutput(const std::filesystem::path& WorkingDirectory)
	{
		std::cout << "PLAYER-STDOUT-OK\nPLAYER-CWD:" << PathToUtf8(WorkingDirectory) << '\n' << std::flush;
		std::cerr << "PLAYER-STDERR-OK\n" << std::flush;
		std::cout << "PLAYER-FINAL-NO-NEWLINE" << std::flush;
	}

	void TestEditorPlayerExecutablePath(TestRunner& Tests)
	{
		const std::filesystem::path OutputDirectory = std::filesystem::temp_directory_path() / "PulseForge Player Ω";
		const std::filesystem::path EditorExecutable = OutputDirectory / "PulseForgeEditor.exe";
		auto WindowsPath = PulseForgeEditor::ResolveEditorPlayerExecutablePath(
			EditorExecutable, PulseForgeEditor::EditorPlayerPlatform::Windows);
		PF_CHECK(Tests, WindowsPath && *WindowsPath == EditorExecutable.parent_path() / "PulseForgeGame.exe");

		auto LinuxPath = PulseForgeEditor::ResolveEditorPlayerExecutablePath(
			EditorExecutable, PulseForgeEditor::EditorPlayerPlatform::Linux);
		PF_CHECK(Tests, LinuxPath && *LinuxPath == EditorExecutable.parent_path() / "PulseForgeGame");

		const std::filesystem::path MacEditorExecutable = OutputDirectory / "PulseForgeEditor.app" / "Contents" / "MacOS" /
			"PulseForgeEditor";
		auto MacPath = PulseForgeEditor::ResolveEditorPlayerExecutablePath(
			MacEditorExecutable, PulseForgeEditor::EditorPlayerPlatform::MacOS);
		PF_CHECK(Tests, MacPath && *MacPath == OutputDirectory / "PulseForgeGame.app" / "Contents" / "MacOS" / "PulseForgeGame");

		PF_CHECK(Tests, !PulseForgeEditor::ResolveEditorPlayerExecutablePath({}, PulseForgeEditor::EditorPlayerPlatform::Windows));
		PF_CHECK(Tests, !PulseForgeEditor::ResolveEditorPlayerExecutablePath(
			"PulseForgeEditor.exe", PulseForgeEditor::EditorPlayerPlatform::Windows));
		PF_CHECK(Tests, !PulseForgeEditor::ResolveEditorPlayerExecutablePath(
			EditorExecutable, static_cast<PulseForgeEditor::EditorPlayerPlatform>(255)));

#if defined(PF_PLATFORM_WINDOWS)
		PF_CHECK(Tests, PulseForgeEditor::GetEditorPlayerPlatform() == PulseForgeEditor::EditorPlayerPlatform::Windows);
#elif defined(PF_PLATFORM_LINUX)
		PF_CHECK(Tests, PulseForgeEditor::GetEditorPlayerPlatform() == PulseForgeEditor::EditorPlayerPlatform::Linux);
#elif defined(PF_PLATFORM_MACOS)
		PF_CHECK(Tests, PulseForgeEditor::GetEditorPlayerPlatform() == PulseForgeEditor::EditorPlayerPlatform::MacOS);
#else
#error "Editor player path tests require the configured platform definition."
#endif
	}

	int RunOutputChild(std::string_view Argument)
	{
		if (Argument != ChildArgumentUtf8) return 91;
		std::error_code Error;
		const std::filesystem::path WorkingDirectory = std::filesystem::current_path(Error);
		if (Error) return 92;
		WriteChildOutput(WorkingDirectory);
		return static_cast<int>(SuccessfulChildExitCode);
	}

	int RunWaitingChild()
	{
		std::cout << "PLAYER-WAITING\n" << std::flush;
		for (int Index = 0; Index < 300; ++Index)
			std::this_thread::sleep_for(100ms);
		return static_cast<int>(SuccessfulChildExitCode);
	}

	int RunCooperativeStopChild()
	{
#ifdef _WIN32
		HANDLE Input = GetStdHandle(STD_INPUT_HANDLE);
		for (;;)
		{
			DWORD Available = 0;
			if (!PeekNamedPipe(Input, nullptr, 0, nullptr, &Available, nullptr)) return 93;
			if (Available > 0)
			{
				char Request = 0;
				DWORD Read = 0;
				if (!ReadFile(Input, &Request, 1, &Read, nullptr) || Read != 1 || Request != 'S') return 94;
				break;
			}
			Sleep(2);
		}
#else
		const int Flags = ::fcntl(STDIN_FILENO, F_GETFL);
		if (Flags < 0 || ::fcntl(STDIN_FILENO, F_SETFL, Flags | O_NONBLOCK) < 0) return 93;
		for (;;)
		{
			char Request = 0;
			const ssize_t Read = ::read(STDIN_FILENO, &Request, 1);
			if (Read == 1)
			{
				if (Request != 'S') return 94;
				break;
			}
			if (Read == 0) return 95;
			if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return 96;
			std::this_thread::sleep_for(2ms);
		}
#endif
		std::cout << "PLAYER-COOPERATIVE-CLEANUP-MARKER" << std::flush;
		std::cerr << "PLAYER-CLEANUP-STDERR" << std::flush;
		std::cout << "PULSEFORGE_EDITOR_CONTROL_ACCEPTED" << std::flush;
		return 0;
	}

	int RunUnresponsiveChild()
	{
		std::cout << "PLAYER-UNRESPONSIVE" << std::flush;
		std::this_thread::sleep_for(10s);
		return 0;
	}

	int RunLargeOutputChild()
	{
		std::string Payload(128 * 1024, 'x');
		std::cout.write(Payload.data(), static_cast<std::streamsize>(Payload.size()));
		std::cout << "PLAYER-LARGE-FINAL" << std::flush;
		std::cerr << "PLAYER-LARGE-STDERR" << std::flush;
		return 0;
	}

	int RunStartupFailureChild()
	{
		std::cerr << "PLAYER-STARTUP-FAILURE" << std::flush;
		return 17;
	}

	int RunExitWithBufferedOutputChild(const std::filesystem::path& Gate)
	{
		std::cout << "PLAYER-BUFFER-READY" << std::flush;
		const auto Deadline = std::chrono::steady_clock::now() + 10s;
		while (!std::filesystem::exists(Gate) && std::chrono::steady_clock::now() < Deadline)
			std::this_thread::sleep_for(1ms);
		if (!std::filesystem::exists(Gate)) return 18;
		std::string FinalBytes(1024, 'b');
		FinalBytes += "PLAYER-EXIT-BUFFERED-FINAL";
		std::cout.write(FinalBytes.data(), static_cast<std::streamsize>(FinalBytes.size())) << std::flush;
		return 0;
	}

	int RunDescendantWriterParent(const std::filesystem::path& Executable, const std::filesystem::path& Gate)
	{
		std::cout << "PLAYER-PARENT-EXITED" << std::flush;
#ifdef _WIN32
		std::wstring CommandLine = L"\"" + Executable.native() + L"\" --pulseforge-player-process-hold-writer \"" + Gate.native() + L"\"";
		std::vector<wchar_t> MutableCommandLine(CommandLine.begin(), CommandLine.end());
		MutableCommandLine.push_back(L'\0');
		STARTUPINFOW Startup{};
		Startup.cb = sizeof(Startup);
		Startup.dwFlags = STARTF_USESTDHANDLES;
		Startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
		Startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
		Startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
		PROCESS_INFORMATION Process{};
		if (!CreateProcessW(Executable.c_str(), MutableCommandLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
				&Startup, &Process))
			return 19;
		CloseHandle(Process.hThread);
		CloseHandle(Process.hProcess);
#else
		const pid_t Descendant = ::fork();
		if (Descendant < 0) return 19;
		if (Descendant == 0)
		{
			std::this_thread::sleep_for(2300ms);
			std::ofstream(Gate, std::ios::binary).put('1');
			::_exit(0);
		}
#endif
		return 0;
	}

	int RunHoldWriterChild(const std::filesystem::path& Gate)
	{
		std::this_thread::sleep_for(2300ms);
		std::ofstream(Gate, std::ios::binary).put('1');
		return 0;
	}

	struct TemporaryDirectory final
	{
		std::filesystem::path Root;
		TemporaryDirectory(const TemporaryDirectory&) = delete;
		TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
		TemporaryDirectory(TemporaryDirectory&& Other) noexcept : Root(std::move(Other.Root)) { Other.Root.clear(); }
		TemporaryDirectory& operator=(TemporaryDirectory&&) = delete;
		explicit TemporaryDirectory(std::filesystem::path Value) : Root(std::move(Value)) {}
		~TemporaryDirectory()
		{
			std::error_code Error;
			std::filesystem::remove_all(Root, Error);
		}
	};

	std::expected<TemporaryDirectory, std::string> CreateTemporaryWorkingDirectory()
	{
		std::error_code Error;
		const std::filesystem::path TemporaryRoot = std::filesystem::temp_directory_path(Error);
		if (Error) return std::unexpected("Could not locate the temporary directory: " + Error.message());

		const auto Timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
		const std::string TimestampText = std::to_string(Timestamp);
		std::u8string DirectoryName = u8"PulseForge Player Process cwd \u03A9 \U0001F680 ";
		DirectoryName.append(reinterpret_cast<const char8_t*>(TimestampText.data()), TimestampText.size());
		for (uint32_t Attempt = 0; Attempt < 16; ++Attempt)
		{
			std::u8string CandidateName = DirectoryName;
			CandidateName.push_back(u8'-');
			const std::string AttemptText = std::to_string(Attempt);
			CandidateName.append(reinterpret_cast<const char8_t*>(AttemptText.data()), AttemptText.size());
			const std::filesystem::path Candidate = TemporaryRoot / CandidateName;
			if (std::filesystem::create_directory(Candidate, Error)) return TemporaryDirectory{ Candidate };
			if (Error && Error != std::errc::file_exists)
				return std::unexpected("Could not create the Unicode player working directory: " + Error.message());
			Error.clear();
		}
		return std::unexpected("Could not reserve a unique Unicode player working directory");
	}

	struct ProcessPollResult final
	{
		bool Exited = false;
		std::optional<int64_t> ExitCode;
		std::string Output;
		std::string Error;
		bool OutputComplete = false;
		bool OutputTruncated = false;
		bool CooperativeStop = false;
		bool ForcedTermination = false;
	};

	ProcessPollResult PollUntilExit(PulseForgeEditor::EditorPlayerProcess& Process, std::chrono::seconds Deadline)
	{
		const auto StopAt = std::chrono::steady_clock::now() + Deadline;
		ProcessPollResult Result;
		while (std::chrono::steady_clock::now() < StopAt)
		{
			auto Update = Process.Poll(16 * 1024);
			Result.Output += Update.OutputChunk;
			if (Update.Error) Result.Error = *Update.Error;
			Result.OutputComplete = Update.OutputComplete;
			Result.OutputTruncated = Update.OutputTruncated;
			Result.CooperativeStop = Update.CooperativeStopObserved;
			Result.ForcedTermination = Update.ForcedTermination;
			if (!Update.Running && Update.OutputComplete)
			{
				Result.Exited = true;
				Result.ExitCode = Update.ExitCode;
				return Result;
			}
			std::this_thread::sleep_for(5ms);
		}
		Result.Error = "Timed out while waiting for the player process";
		return Result;
	}

	void TestPlayerLaunchCapturesArgumentsOutputAndExit(TestRunner& Tests, const std::filesystem::path& Executable)
	{
		auto WorkingDirectory = CreateTemporaryWorkingDirectory();
		PF_CHECK(Tests, WorkingDirectory.has_value());
		if (!WorkingDirectory) return;

		PulseForgeEditor::EditorPlayerLaunchSpec Spec;
		Spec.Executable = Executable;
		Spec.WorkingDirectory = WorkingDirectory->Root;
		Spec.Arguments = { "--pulseforge-player-process-emit", std::string(ChildArgumentUtf8) };
		auto Launched = PulseForgeEditor::EditorPlayerProcess::Launch(Spec);
		PF_CHECK(Tests, Launched.has_value());
		if (!Launched) return;

		const ProcessPollResult Result = PollUntilExit(**Launched, 10s);
		PF_CHECK(Tests, Result.Error.empty());
		PF_CHECK(Tests, Result.Exited);
		PF_CHECK(Tests, Result.ExitCode == SuccessfulChildExitCode);
		const std::string Output = NormalizeNewlines(Result.Output);
		PF_CHECK(Tests, Output.find("PLAYER-STDOUT-OK\n") != std::string::npos);
		PF_CHECK(Tests, Output.find("PLAYER-STDERR-OK\n") != std::string::npos);
		PF_CHECK(Tests, Output.ends_with("PLAYER-FINAL-NO-NEWLINE"));
		const std::string WorkingDirectoryPrefix = "PLAYER-CWD:";
		const size_t WorkingDirectoryStart = Output.find(WorkingDirectoryPrefix);
		PF_CHECK(Tests, WorkingDirectoryStart != std::string::npos);
		if (WorkingDirectoryStart != std::string::npos)
		{
			const size_t WorkingDirectoryEnd = Output.find('\n', WorkingDirectoryStart);
			PF_CHECK(Tests, WorkingDirectoryEnd != std::string::npos);
			if (WorkingDirectoryEnd != std::string::npos)
			{
				const std::string_view ReportedUtf8(Output.data() + WorkingDirectoryStart + WorkingDirectoryPrefix.size(),
					WorkingDirectoryEnd - WorkingDirectoryStart - WorkingDirectoryPrefix.size());
				std::error_code Error;
				const bool IsSameDirectory = std::filesystem::equivalent(
					WorkingDirectory->Root, PathFromUtf8(ReportedUtf8), Error);
				PF_CHECK(Tests, !Error);
				PF_CHECK(Tests, IsSameDirectory);
			}
		}
	}

	void TestMissingExecutableFailsImmediately(TestRunner& Tests, const std::filesystem::path& WorkingDirectory)
	{
		PulseForgeEditor::EditorPlayerLaunchSpec Spec;
		Spec.Executable = WorkingDirectory / "Missing Player Executable Ω";
		Spec.WorkingDirectory = WorkingDirectory;
		const auto Result = PulseForgeEditor::EditorPlayerProcess::Launch(Spec);
		PF_CHECK(Tests, !Result);
		PF_CHECK(Tests, !Result && !Result.error().empty());
	}

	void TestStopTargetsOnlyOwnedChild(
		TestRunner& Tests, const std::filesystem::path& Executable, const std::filesystem::path& WorkingDirectory)
	{
		PulseForgeEditor::EditorPlayerLaunchSpec Spec;
		Spec.Executable = Executable;
		Spec.WorkingDirectory = WorkingDirectory;
		Spec.Arguments = { "--pulseforge-player-process-cooperative-stop" };
		auto Owned = PulseForgeEditor::EditorPlayerProcess::Launch(Spec);
		auto Unrelated = PulseForgeEditor::EditorPlayerProcess::Launch(Spec);
		PF_CHECK(Tests, Owned.has_value());
		PF_CHECK(Tests, Unrelated.has_value());
		if (!Owned || !Unrelated) return;

		const auto PollStarted = std::chrono::steady_clock::now();
		auto OwnedUpdate = (*Owned)->Poll();
		const auto PollDuration = std::chrono::steady_clock::now() - PollStarted;
		auto UnrelatedUpdate = (*Unrelated)->Poll();
		PF_CHECK(Tests, PollDuration < 50ms);
		PF_CHECK(Tests, OwnedUpdate.Running);
		PF_CHECK(Tests, UnrelatedUpdate.Running);

		(*Owned)->RequestStop();
		(*Owned)->RequestStop();
		const ProcessPollResult OwnedResult = PollUntilExit(**Owned, 5s);
		PF_CHECK(Tests, OwnedResult.Error.empty());
		PF_CHECK(Tests, OwnedResult.Exited);
		PF_CHECK(Tests, OwnedResult.ExitCode != SuccessfulChildExitCode);

		UnrelatedUpdate = (*Unrelated)->Poll();
		PF_CHECK(Tests, UnrelatedUpdate.Running);
		(*Unrelated)->RequestStop();
		const ProcessPollResult UnrelatedResult = PollUntilExit(**Unrelated, 5s);
		PF_CHECK(Tests, UnrelatedResult.Exited);
	}

	void TestCooperativeStopRunsChildCleanup(TestRunner& Tests, const std::filesystem::path& Executable,
		const std::filesystem::path& WorkingDirectory)
	{
		PulseForgeEditor::EditorPlayerLaunchSpec Spec{ .Executable = Executable, .WorkingDirectory = WorkingDirectory,
			.Arguments = { "--pulseforge-player-process-cooperative-stop" } };
		auto Launched = PulseForgeEditor::EditorPlayerProcess::Launch(Spec);
		PF_CHECK(Tests, Launched.has_value());
		if (!Launched) return;
		(*Launched)->RequestStop();
		(*Launched)->RequestStop();
		const ProcessPollResult Result = PollUntilExit(**Launched, 5s);
		PF_CHECK(Tests, Result.Error.empty());
		PF_CHECK(Tests, Result.Exited && Result.ExitCode == 0);
		PF_CHECK(Tests, Result.CooperativeStop && !Result.ForcedTermination);
		PF_CHECK(Tests, Result.Output.find("PLAYER-COOPERATIVE-CLEANUP-MARKER") != std::string::npos);
		PF_CHECK(Tests, Result.Output.find("PLAYER-CLEANUP-STDERR") != std::string::npos);
	}

	void TestUnresponsiveChildIsClassifiedAsForced(TestRunner& Tests, const std::filesystem::path& Executable,
		const std::filesystem::path& WorkingDirectory)
	{
		PulseForgeEditor::EditorPlayerLaunchSpec Spec{ .Executable = Executable, .WorkingDirectory = WorkingDirectory,
			.Arguments = { "--pulseforge-player-process-unresponsive" } };
		auto Launched = PulseForgeEditor::EditorPlayerProcess::Launch(Spec);
		PF_CHECK(Tests, Launched.has_value());
		if (!Launched) return;
		const auto StopAt = std::chrono::steady_clock::now();
		(*Launched)->RequestStop();
		const ProcessPollResult Result = PollUntilExit(**Launched, 6s);
		const auto StopDuration = std::chrono::steady_clock::now() - StopAt;
		PF_CHECK(Tests, Result.Exited && Result.ExitCode.has_value());
		PF_CHECK(Tests, StopDuration >= 2500ms);
		PF_CHECK(Tests, Result.ForcedTermination && !Result.CooperativeStop);
	}

	void TestLargeOutputDrainsAfterExit(TestRunner& Tests, const std::filesystem::path& Executable,
		const std::filesystem::path& WorkingDirectory)
	{
		PulseForgeEditor::EditorPlayerLaunchSpec Spec{ .Executable = Executable, .WorkingDirectory = WorkingDirectory,
			.Arguments = { "--pulseforge-player-process-large-output" } };
		auto Launched = PulseForgeEditor::EditorPlayerProcess::Launch(Spec);
		PF_CHECK(Tests, Launched.has_value());
		if (!Launched) return;
		const ProcessPollResult Result = PollUntilExit(**Launched, 5s);
		PF_CHECK(Tests, Result.Exited && Result.ExitCode == 0);
		PF_CHECK(Tests, Result.OutputComplete);
		PF_CHECK(Tests, Result.Output.size() >= 128 * 1024);
		PF_CHECK(Tests, Result.Output.find("PLAYER-LARGE-FINAL") != std::string::npos);
		PF_CHECK(Tests, Result.Output.find("PLAYER-LARGE-STDERR") != std::string::npos);
	}

	void TestEarlyStartupFailureRemainsDistinct(TestRunner& Tests, const std::filesystem::path& Executable,
		const std::filesystem::path& WorkingDirectory)
	{
		PulseForgeEditor::EditorPlayerLaunchSpec Spec{ .Executable = Executable, .WorkingDirectory = WorkingDirectory,
			.Arguments = { "--pulseforge-player-process-startup-failure" } };
		auto Launched = PulseForgeEditor::EditorPlayerProcess::Launch(Spec);
		PF_CHECK(Tests, Launched.has_value());
		if (!Launched) return;
		const ProcessPollResult Result = PollUntilExit(**Launched, 5s);
		PF_CHECK(Tests, Result.Exited && Result.ExitCode == 17);
		PF_CHECK(Tests, Result.Output.find("PLAYER-STARTUP-FAILURE") != std::string::npos);
		(*Launched)->RequestStop();
		const auto AfterStop = (*Launched)->Poll();
		PF_CHECK(Tests, !AfterStop.Running && AfterStop.ExitCode == 17);
		PF_CHECK(Tests, !AfterStop.CooperativeStopObserved && !AfterStop.ForcedTermination);
	}

	void TestExitIsReportedBeforeBufferedOutputCompletes(TestRunner& Tests, const std::filesystem::path& Executable,
		const std::filesystem::path& WorkingDirectory)
	{
		const std::filesystem::path Gate = WorkingDirectory / "release gate";
		PulseForgeEditor::EditorPlayerLaunchSpec Spec{ .Executable = Executable, .WorkingDirectory = WorkingDirectory,
			.Arguments = { "--pulseforge-player-process-exit-buffered", PathToUtf8(Gate) } };
		auto Launched = PulseForgeEditor::EditorPlayerProcess::Launch(Spec);
		PF_CHECK(Tests, Launched.has_value());
		if (!Launched) return;
		std::string Output;
		const auto Deadline = std::chrono::steady_clock::now() + 5s;
		while (std::chrono::steady_clock::now() < Deadline && Output.find("PLAYER-BUFFER-READY") == std::string::npos)
		{
			auto Update = (*Launched)->Poll();
			Output += Update.OutputChunk;
			if (Update.Error) PF_CHECK(Tests, false);
			std::this_thread::sleep_for(1ms);
		}
		PF_CHECK(Tests, Output.find("PLAYER-BUFFER-READY") != std::string::npos);
		std::ofstream(Gate, std::ios::binary).put('1');
		bool SawExitedWhileOutputPending = false;
		bool Completed = false;
		std::optional<int64_t> ExitCode;
		while (std::chrono::steady_clock::now() < Deadline && !Completed)
		{
			const auto Update = (*Launched)->Poll(1);
			Output += Update.OutputChunk;
			if (Update.Error) PF_CHECK(Tests, false);
			if (!Update.Running && !Update.OutputComplete) SawExitedWhileOutputPending = true;
			if (!Update.Running && Update.OutputComplete)
			{
				Completed = true;
				ExitCode = Update.ExitCode;
			}
		}
		PF_CHECK(Tests, Completed && ExitCode == 0);
		PF_CHECK(Tests, SawExitedWhileOutputPending);
		PF_CHECK(Tests, Output.find("PLAYER-EXIT-BUFFERED-FINAL") != std::string::npos);
	}

	void TestDescendantWriterHasBoundedDrainAbandonment(TestRunner& Tests, const std::filesystem::path& Executable,
		const std::filesystem::path& WorkingDirectory)
	{
		const std::filesystem::path Gate = WorkingDirectory / "writer release marker";
		PulseForgeEditor::EditorPlayerLaunchSpec Spec{ .Executable = Executable, .WorkingDirectory = WorkingDirectory,
			.Arguments = { "--pulseforge-player-process-descendant-writer", PathToUtf8(Gate) } };
		auto Launched = PulseForgeEditor::EditorPlayerProcess::Launch(Spec);
		PF_CHECK(Tests, Launched.has_value());
		if (!Launched) return;
		const ProcessPollResult Result = PollUntilExit(**Launched, 6s);
		PF_CHECK(Tests, Result.Exited && Result.ExitCode == 0);
		PF_CHECK(Tests, Result.OutputComplete && Result.ForcedTermination == false);
		PF_CHECK(Tests, Result.OutputTruncated);
		PF_CHECK(Tests, Result.Output.find("PLAYER-PARENT-EXITED") != std::string::npos);
		const auto WriterDeadline = std::chrono::steady_clock::now() + 2s;
		while (!std::filesystem::exists(Gate) && std::chrono::steady_clock::now() < WriterDeadline)
			std::this_thread::sleep_for(5ms);
		PF_CHECK(Tests, std::filesystem::exists(Gate));
	}

	void TestProductionPlayerCooperativeShutdown(TestRunner& Tests, std::string_view PlayerPath, std::string_view ProjectPath)
	{
		PulseForgeEditor::EditorPlayerLaunchSpec Spec;
		Spec.Executable = PathFromUtf8(PlayerPath);
		Spec.WorkingDirectory = Spec.Executable.parent_path();
		Spec.Arguments = { "--project", std::string(ProjectPath), "--editor-control-stdin" };
		auto Launched = PulseForgeEditor::EditorPlayerProcess::Launch(Spec);
		PF_CHECK(Tests, Launched.has_value());
		if (!Launched) return;

		std::string Output;
		const auto StartupDeadline = std::chrono::steady_clock::now() + 30s;
		while (std::chrono::steady_clock::now() < StartupDeadline && Output.find("Submitted ") == std::string::npos)
		{
			const auto Update = (*Launched)->Poll();
			Output += Update.OutputChunk;
			if (Update.Error) PF_CHECK(Tests, false);
			if (!Update.Running) break;
			std::this_thread::sleep_for(5ms);
		}
		const bool RenderedScene = Output.find("Submitted ") != std::string::npos;
		PF_CHECK(Tests, RenderedScene);
		(*Launched)->RequestStop();

		const auto ShutdownDeadline = std::chrono::steady_clock::now() + 10s;
		PulseForgeEditor::EditorPlayerProcessUpdate FinalUpdate;
		while (std::chrono::steady_clock::now() < ShutdownDeadline)
		{
			FinalUpdate = (*Launched)->Poll();
			Output += FinalUpdate.OutputChunk;
			if (FinalUpdate.Error) PF_CHECK(Tests, false);
			if (!FinalUpdate.Running && FinalUpdate.OutputComplete) break;
			std::this_thread::sleep_for(5ms);
		}
		PF_CHECK(Tests, !FinalUpdate.Running && FinalUpdate.OutputComplete);
		PF_CHECK(Tests, FinalUpdate.ExitCode == 0);
		PF_CHECK(Tests, FinalUpdate.CooperativeStopObserved && !FinalUpdate.ForcedTermination);
		PF_CHECK(Tests, Output.find("PULSEFORGE_PLAYER_ORDERLY_STOP=PASS") != std::string::npos);
	}

	int RunHarness(const std::filesystem::path& Executable)
	{
		TestRunner Tests;
		TestEditorPlayerExecutablePath(Tests);
		auto WorkingDirectory = CreateTemporaryWorkingDirectory();
		PF_CHECK(Tests, WorkingDirectory.has_value());
		if (WorkingDirectory)
		{
			TestPlayerLaunchCapturesArgumentsOutputAndExit(Tests, Executable);
			TestMissingExecutableFailsImmediately(Tests, WorkingDirectory->Root);
			TestStopTargetsOnlyOwnedChild(Tests, Executable, WorkingDirectory->Root);
			TestCooperativeStopRunsChildCleanup(Tests, Executable, WorkingDirectory->Root);
			TestUnresponsiveChildIsClassifiedAsForced(Tests, Executable, WorkingDirectory->Root);
			TestLargeOutputDrainsAfterExit(Tests, Executable, WorkingDirectory->Root);
			TestEarlyStartupFailureRemainsDistinct(Tests, Executable, WorkingDirectory->Root);
			TestExitIsReportedBeforeBufferedOutputCompletes(Tests, Executable, WorkingDirectory->Root);
			TestDescendantWriterHasBoundedDrainAbandonment(Tests, Executable, WorkingDirectory->Root);
			const char* PlayerExecutable = std::getenv("PULSEFORGE_PLAYER_INTEGRATION_EXECUTABLE");
			const char* PlayerProject = std::getenv("PULSEFORGE_PLAYER_INTEGRATION_PROJECT");
			if (PlayerExecutable && PlayerProject)
				TestProductionPlayerCooperativeShutdown(Tests, PlayerExecutable, PlayerProject);
		}
		return Tests.Finish();
	}

#ifdef _WIN32
	int RunChildMode(int ArgumentCount, wchar_t** Arguments)
	{
		if (ArgumentCount >= 3 && std::wstring_view(Arguments[1]) == L"--pulseforge-player-process-emit")
		{
			if (std::wstring_view(Arguments[2]) != L"argument with spaces \u03A9 \U0001F680") return 91;
			return RunOutputChild(std::string(ChildArgumentUtf8));
		}
		if (ArgumentCount >= 2 && std::wstring_view(Arguments[1]) == L"--pulseforge-player-process-wait") return RunWaitingChild();
		if (ArgumentCount >= 2 && std::wstring_view(Arguments[1]) == L"--pulseforge-player-process-cooperative-stop") return RunCooperativeStopChild();
		if (ArgumentCount >= 2 && std::wstring_view(Arguments[1]) == L"--pulseforge-player-process-unresponsive") return RunUnresponsiveChild();
		if (ArgumentCount >= 2 && std::wstring_view(Arguments[1]) == L"--pulseforge-player-process-large-output") return RunLargeOutputChild();
		if (ArgumentCount >= 2 && std::wstring_view(Arguments[1]) == L"--pulseforge-player-process-startup-failure") return RunStartupFailureChild();
		if (ArgumentCount >= 3 && std::wstring_view(Arguments[1]) == L"--pulseforge-player-process-exit-buffered")
			return RunExitWithBufferedOutputChild(std::filesystem::path(Arguments[2]));
		if (ArgumentCount >= 3 && std::wstring_view(Arguments[1]) == L"--pulseforge-player-process-descendant-writer")
			return RunDescendantWriterParent(std::filesystem::path(Arguments[0]), std::filesystem::path(Arguments[2]));
		if (ArgumentCount >= 3 && std::wstring_view(Arguments[1]) == L"--pulseforge-player-process-hold-writer")
			return RunHoldWriterChild(std::filesystem::path(Arguments[2]));
		return -1;
	}
#else
	int RunChildMode(int ArgumentCount, char** Arguments)
	{
		if (ArgumentCount >= 3 && std::string_view(Arguments[1]) == "--pulseforge-player-process-emit") return RunOutputChild(Arguments[2]);
		if (ArgumentCount >= 2 && std::string_view(Arguments[1]) == "--pulseforge-player-process-wait") return RunWaitingChild();
		if (ArgumentCount >= 2 && std::string_view(Arguments[1]) == "--pulseforge-player-process-cooperative-stop") return RunCooperativeStopChild();
		if (ArgumentCount >= 2 && std::string_view(Arguments[1]) == "--pulseforge-player-process-unresponsive") return RunUnresponsiveChild();
		if (ArgumentCount >= 2 && std::string_view(Arguments[1]) == "--pulseforge-player-process-large-output") return RunLargeOutputChild();
		if (ArgumentCount >= 2 && std::string_view(Arguments[1]) == "--pulseforge-player-process-startup-failure") return RunStartupFailureChild();
		if (ArgumentCount >= 3 && std::string_view(Arguments[1]) == "--pulseforge-player-process-exit-buffered")
			return RunExitWithBufferedOutputChild(PathFromUtf8(Arguments[2]));
		if (ArgumentCount >= 3 && std::string_view(Arguments[1]) == "--pulseforge-player-process-descendant-writer")
			return RunDescendantWriterParent(PathFromUtf8(Arguments[0]), PathFromUtf8(Arguments[2]));
		if (ArgumentCount >= 3 && std::string_view(Arguments[1]) == "--pulseforge-player-process-hold-writer")
			return RunHoldWriterChild(PathFromUtf8(Arguments[2]));
		return -1;
	}
#endif
} // namespace

#ifdef _WIN32
int wmain(int ArgumentCount, wchar_t** Arguments)
{
	const int ChildResult = RunChildMode(ArgumentCount, Arguments);
	if (ChildResult >= 0) return ChildResult;
	std::error_code Error;
	const std::filesystem::path Executable = std::filesystem::absolute(Arguments[0], Error);
	if (Error)
	{
		std::cerr << "Could not resolve the process-test executable path: " << Error.message() << '\n';
		return 1;
	}
	return RunHarness(Executable);
}
#else
int main(int ArgumentCount, char** Arguments)
{
	const int ChildResult = RunChildMode(ArgumentCount, Arguments);
	if (ChildResult >= 0) return ChildResult;
	std::error_code Error;
	const std::filesystem::path Executable = std::filesystem::absolute(Arguments[0], Error);
	if (Error)
	{
		std::cerr << "Could not resolve the process-test executable path: " << Error.message() << '\n';
		return 1;
	}
	return RunHarness(Executable);
}
#endif
