#include "Core/Log.h"
#include "Editor/EditorCommandLine.h"
#include "EditorApplicationFactory.h"

#include <cstdlib>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

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

		std::error_code CurrentDirectoryError;
		const std::filesystem::path WorkingDirectory = std::filesystem::current_path(CurrentDirectoryError);
		if (CurrentDirectoryError)
			throw std::runtime_error("The editor working directory could not be resolved: " + CurrentDirectoryError.message());

		const auto Options = PulseForgeEditor::ParseEditorCommandLine(CommandLineArguments, WorkingDirectory);
		if (!Options)
		{
			std::cerr << "PulseForgeEditor: " << Options.error().Message << '\n';
			PF_CORE_CRITICAL("Editor command-line error: {0}", Options.error().Message);
			return EXIT_FAILURE;
		}
		#ifndef _DEBUG
		if (Options->EnableDocumentHostTestFixture)
		{
			std::cerr << "PulseForgeEditor: --document-host-test-fixture is available only in Debug builds.\n";
			return EXIT_FAILURE;
		}
		#endif

		auto Application = PulseForgeEditor::CreateEditorApplication(*Options);
		if (!Application)
			throw std::runtime_error("The editor application factory returned null");

		PF_INFO("PulseForge editor application created");
		Application->Run();
		return EXIT_SUCCESS;
	}
	catch (const std::exception& Exception)
	{
		std::cerr << "PulseForgeEditor startup/runtime failure: " << Exception.what() << '\n';
		PF_CORE_CRITICAL("Application startup/runtime failure: {0}", Exception.what());
		return EXIT_FAILURE;
	}
}
