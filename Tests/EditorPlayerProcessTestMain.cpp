#include "Editor/EditorPlayerProcess.h"
#include "TestSupport.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

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
			if (!Update.Running)
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
		Spec.Arguments = { "--pulseforge-player-process-wait" };
		auto Owned = PulseForgeEditor::EditorPlayerProcess::Launch(Spec);
		auto Unrelated = PulseForgeEditor::EditorPlayerProcess::Launch(Spec);
		PF_CHECK(Tests, Owned.has_value());
		PF_CHECK(Tests, Unrelated.has_value());
		if (!Owned || !Unrelated) return;

		auto OwnedUpdate = (*Owned)->Poll();
		auto UnrelatedUpdate = (*Unrelated)->Poll();
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

	int RunHarness(const std::filesystem::path& Executable)
	{
		TestRunner Tests;
		auto WorkingDirectory = CreateTemporaryWorkingDirectory();
		PF_CHECK(Tests, WorkingDirectory.has_value());
		if (WorkingDirectory)
		{
			TestPlayerLaunchCapturesArgumentsOutputAndExit(Tests, Executable);
			TestMissingExecutableFailsImmediately(Tests, WorkingDirectory->Root);
			TestStopTargetsOnlyOwnedChild(Tests, Executable, WorkingDirectory->Root);
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
		return -1;
	}
#else
	int RunChildMode(int ArgumentCount, char** Arguments)
	{
		if (ArgumentCount >= 3 && std::string_view(Arguments[1]) == "--pulseforge-player-process-emit") return RunOutputChild(Arguments[2]);
		if (ArgumentCount >= 2 && std::string_view(Arguments[1]) == "--pulseforge-player-process-wait") return RunWaitingChild();
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
