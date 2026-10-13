#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace PulseForgeEditor
{
	enum class EditorCommandLineErrorCode : uint8_t
	{
		MissingProjectPath,
		DuplicateProjectOption,
		UnexpectedArgument,
		InvalidProjectPath
	};

	struct EditorCommandLineError
	{
		EditorCommandLineErrorCode Code;
		std::string Message;
	};

	struct EditorCommandLineOptions
	{
		std::optional<std::filesystem::path> ProjectFile;
	};

	[[nodiscard]] inline std::expected<EditorCommandLineOptions, EditorCommandLineError> ParseEditorCommandLine(
		std::span<const std::string_view> Arguments,
		const std::filesystem::path& WorkingDirectory)
	{
		EditorCommandLineOptions Options;
		for (std::size_t Index = 0; Index < Arguments.size(); ++Index)
		{
			if (Arguments[Index] != "--project")
				return std::unexpected(EditorCommandLineError{
					EditorCommandLineErrorCode::UnexpectedArgument,
					"Unexpected argument '" + std::string(Arguments[Index]) + "'. Supported option: --project <path>." });

			if (Options.ProjectFile)
				return std::unexpected(EditorCommandLineError{
					EditorCommandLineErrorCode::DuplicateProjectOption,
					"The --project option may be specified only once." });

			if (Index + 1 >= Arguments.size() || Arguments[Index + 1].starts_with("--"))
				return std::unexpected(EditorCommandLineError{
					EditorCommandLineErrorCode::MissingProjectPath,
					"The --project option requires a .pfproj path." });

			const std::string_view SuppliedPath = Arguments[++Index];
			std::error_code Error;
			std::filesystem::path ProjectFile = std::filesystem::u8path(SuppliedPath);
			if (ProjectFile.is_relative())
				ProjectFile = WorkingDirectory / ProjectFile;
			ProjectFile = std::filesystem::absolute(ProjectFile, Error).lexically_normal();
			if (Error)
				return std::unexpected(EditorCommandLineError{
					EditorCommandLineErrorCode::InvalidProjectPath,
					"The project path could not be resolved: " + Error.message() });

			const auto Extension = ProjectFile.extension().string();
			std::string NormalizedExtension = Extension;
			std::transform(NormalizedExtension.begin(), NormalizedExtension.end(), NormalizedExtension.begin(),
				[](unsigned char Character)
				{
					return static_cast<char>(std::tolower(Character));
				});
			if (NormalizedExtension != ".pfproj" ||
				!std::filesystem::is_regular_file(ProjectFile, Error) || Error)
			{
				std::string Message = "The project path is not an existing .pfproj file: ";
				Message += ProjectFile.string();
				return std::unexpected(EditorCommandLineError{
					EditorCommandLineErrorCode::InvalidProjectPath,
					std::move(Message) });
			}

			Options.ProjectFile = std::move(ProjectFile);
		}

		return Options;
	}
}
