#include "Core/BoundedInput.h"
#include "Core/PlatformServices.h"
#include "Scene/UUID.h"

#include <filesystem>
#include <iostream>
#include <sstream>
#include <string_view>

int main(int ArgumentCount, char** Arguments)
{
	bool RequireDistinctWorkingDirectory = false;
	if (ArgumentCount == 2 && std::string_view(Arguments[1]) == "--require-distinct-working-directory")
		RequireDistinctWorkingDirectory = true;
	else if (ArgumentCount != 1)
	{
		std::cerr << "Core consumer received an unsupported argument\n";
		return 11;
	}

	const auto ExecutablePath = PulseForge::Platform::GetExecutablePath();
	if (!ExecutablePath || ExecutablePath->empty())
	{
		std::cerr << "Core consumer could not resolve its executable path\n";
		return 1;
	}
	if (!ExecutablePath->is_absolute() || ExecutablePath->filename().empty())
	{
		std::cerr << "Core consumer executable path must be absolute and name a file\n";
		return 7;
	}

	std::error_code FileError;
	if (!std::filesystem::is_regular_file(*ExecutablePath, FileError) || FileError)
	{
		std::cerr << "Core consumer executable path does not resolve to a regular file: " << ExecutablePath->string() << '\n';
		return 8;
	}

	std::error_code CurrentPathError;
	const std::filesystem::path CurrentPath = std::filesystem::current_path(CurrentPathError);
	if (CurrentPathError)
	{
		std::cerr << "Core consumer could not query its current working directory: " << CurrentPathError.message() << '\n';
		return 9;
	}
	if (RequireDistinctWorkingDirectory && ExecutablePath->parent_path().lexically_normal() == CurrentPath.lexically_normal())
	{
		std::cerr << "Core consumer expected a working directory different from its executable directory\n";
		return 10;
	}

	const auto ResourceDirectory = PulseForge::Platform::GetResourceDirectory();
	if (!ResourceDirectory || !ResourceDirectory->is_absolute())
	{
		std::cerr << "Core consumer resource directory must resolve to an absolute path";
		if (!ResourceDirectory) std::cerr << ": " << ResourceDirectory.error().Message;
		std::cerr << '\n';
		return 13;
	}
	std::error_code ResourceDirectoryError;
	if (!std::filesystem::is_directory(*ResourceDirectory, ResourceDirectoryError) || ResourceDirectoryError)
	{
		std::cerr << "Core consumer resource directory does not resolve to a directory: "
			<< ResourceDirectory->string() << '\n';
		return 14;
	}

	const auto UserDataDirectory = PulseForge::Platform::GetUserDataDirectory();
	if (!UserDataDirectory || UserDataDirectory->empty()) return 2;
#if defined(__APPLE__)
	if (!UserDataDirectory->is_absolute() || UserDataDirectory->filename() != "Application Support" ||
		UserDataDirectory->parent_path().filename() != "Library")
	{
		std::cerr << "Core consumer macOS user-data path must resolve under ~/Library/Application Support\n";
		return 12;
	}
#endif

	std::istringstream Input("core consumer");
	const auto Read = PulseForge::Detail::ReadBoundedInput(Input, 32);
	if (!Read || *Read != "core consumer") return 3;

	constexpr std::string_view KnownUUID = "01234567-89ab-cdef-0123-456789abcdef";
	const auto ParsedUUID = PulseForge::UUID::Parse(KnownUUID);
	if (!ParsedUUID || ParsedUUID->ToString() != KnownUUID) return 4;

	const auto GeneratedUUID = PulseForge::UUID::Generate();
	if (!GeneratedUUID || GeneratedUUID->IsNil()) return 5;

	const auto RoundTripUUID = PulseForge::UUID::Parse(GeneratedUUID->ToString());
	if (!RoundTripUUID || *RoundTripUUID != *GeneratedUUID) return 6;

	return 0;
}
