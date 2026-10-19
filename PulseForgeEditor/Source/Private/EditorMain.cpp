#include "Core/Log.h"
#include "Editor/EditorApplicationFactory.h"
#include "Editor/EditorCommandLine.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <shellapi.h>
#else
#include <iostream>
#endif

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
#ifdef _WIN32
	struct LocalArgumentsDeleter
	{
		void operator()(wchar_t** Arguments) const noexcept
		{
			if (Arguments) LocalFree(Arguments);
		}
	};

	[[nodiscard]] std::string ToUtf8(std::wstring_view Argument)
	{
		if (Argument.empty()) return {};
		const int Required = WideCharToMultiByte(
			CP_UTF8, WC_ERR_INVALID_CHARS, Argument.data(), static_cast<int>(Argument.size()), nullptr, 0, nullptr, nullptr);
		if (Required <= 0) throw std::runtime_error("The editor command line contains invalid UTF-16 text");
		std::string Result(static_cast<size_t>(Required), '\0');
		if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, Argument.data(), static_cast<int>(Argument.size()), Result.data(), Required,
				nullptr, nullptr) != Required)
			throw std::runtime_error("The editor command line could not be converted to UTF-8");
		return Result;
	}

	void ReportStartupError(std::string_view Message)
	{
		std::string DebugMessage(Message);
		DebugMessage.push_back('\n');
		OutputDebugStringA(DebugMessage.c_str());

		const int CharacterCount =
			MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Message.data(), static_cast<int>(Message.size()), nullptr, 0);
		if (CharacterCount <= 0)
		{
			MessageBoxA(nullptr, DebugMessage.c_str(), "PulseForge Editor", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
			return;
		}
		std::wstring WideMessage(static_cast<size_t>(CharacterCount), L'\0');
		if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Message.data(), static_cast<int>(Message.size()), WideMessage.data(),
				CharacterCount) != CharacterCount)
		{
			MessageBoxA(nullptr, DebugMessage.c_str(), "PulseForge Editor", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
			return;
		}
		MessageBoxW(nullptr, WideMessage.c_str(), L"PulseForge Editor", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
	}
#else
	void ReportStartupError(std::string_view Message)
	{
		std::cerr << "PulseForge Editor: " << Message << '\n';
	}
#endif

	int RunEditor(std::span<const std::string_view> CommandLineArguments)
	{
		try
		{
			std::error_code CurrentDirectoryError;
			const std::filesystem::path WorkingDirectory = std::filesystem::current_path(CurrentDirectoryError);
			if (CurrentDirectoryError)
				throw std::runtime_error("The editor working directory could not be resolved: " + CurrentDirectoryError.message());

			const auto Options = PulseForgeEditor::ParseEditorCommandLine(CommandLineArguments, WorkingDirectory);
			if (!Options)
			{
				PF_CORE_CRITICAL("Editor command-line error: {0}", Options.error().Message);
				ReportStartupError(Options.error().Message);
				return EXIT_FAILURE;
			}
#if !defined(_DEBUG) && !defined(PF_DEBUG)
			if (Options->EnableDocumentHostTestFixture || Options->ProjectOpenRenderTestFile)
			{
				constexpr std::string_view Error = "Editor runtime test fixtures are available only in Debug builds.";
				PF_CORE_CRITICAL("{0}", Error);
				ReportStartupError(Error);
				return EXIT_FAILURE;
			}
#endif

			auto Application = PulseForgeEditor::CreateEditorApplication(*Options);
			if (!Application) throw std::runtime_error("The editor application factory returned null");

			PF_INFO("PulseForge editor application created");
			Application->Run();
			return EXIT_SUCCESS;
		}
		catch (const std::exception& Exception)
		{
			PF_CORE_CRITICAL("Application startup/runtime failure: {0}", Exception.what());
			ReportStartupError(Exception.what());
			return EXIT_FAILURE;
		}
	}
} // namespace

#ifdef _WIN32
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
	PulseForge::Log::Init();
	PF_CORE_INFO("Engine Logger Initialized!");
	try
	{
		int ArgumentCount = 0;
		std::unique_ptr<wchar_t*, LocalArgumentsDeleter> Arguments(CommandLineToArgvW(GetCommandLineW(), &ArgumentCount));
		if (!Arguments) throw std::runtime_error("The Windows command line could not be read");

		std::vector<std::string> ArgumentStorage;
		ArgumentStorage.reserve(ArgumentCount > 1 ? static_cast<size_t>(ArgumentCount - 1) : 0);
		for (int Index = 1; Index < ArgumentCount; ++Index)
			ArgumentStorage.emplace_back(ToUtf8(Arguments.get()[Index] ? Arguments.get()[Index] : L""));

		std::vector<std::string_view> CommandLineArguments;
		CommandLineArguments.reserve(ArgumentStorage.size());
		for (const std::string& Argument : ArgumentStorage)
			CommandLineArguments.emplace_back(Argument);
		return RunEditor(CommandLineArguments);
	}
	catch (const std::exception& Exception)
	{
		ReportStartupError(Exception.what());
		return EXIT_FAILURE;
	}
}
#else
int main(int ArgumentCount, char** Arguments)
{
	PulseForge::Log::Init();
	PF_CORE_INFO("Engine Logger Initialized!");
	try
	{
		std::vector<std::string_view> CommandLineArguments;
		CommandLineArguments.reserve(ArgumentCount > 1 ? static_cast<size_t>(ArgumentCount - 1) : 0);
		for (int Index = 1; Index < ArgumentCount; ++Index)
			CommandLineArguments.emplace_back(Arguments[Index] ? Arguments[Index] : "");
		return RunEditor(CommandLineArguments);
	}
	catch (const std::exception& Exception)
	{
		PF_CORE_CRITICAL("Application startup/runtime failure: {0}", Exception.what());
		ReportStartupError(Exception.what());
		return EXIT_FAILURE;
	}
}
#endif
