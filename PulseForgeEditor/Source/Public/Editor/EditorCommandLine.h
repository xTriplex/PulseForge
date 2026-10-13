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
		DuplicateOption,
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
		std::optional<std::filesystem::path> AssetPreviewStressManifest;
		std::string AssetPreviewStressBudget;
		bool EnableDocumentHostTestFixture = false;
		bool EnableDocumentHostResetLayoutTest = false;
		bool EnableDocumentHostTabTearOffTest = false;
	};

	[[nodiscard]] inline std::expected<EditorCommandLineOptions, EditorCommandLineError> ParseEditorCommandLine(
		std::span<const std::string_view> Arguments,
		const std::filesystem::path& WorkingDirectory)
	{
		EditorCommandLineOptions Options;
		for (std::size_t Index = 0; Index < Arguments.size(); ++Index)
		{
			if (Arguments[Index] == "--document-host-test-fixture")
			{
				if (Options.EnableDocumentHostTestFixture)
					return std::unexpected(EditorCommandLineError{
						EditorCommandLineErrorCode::DuplicateOption,
						"The --document-host-test-fixture option may be specified only once." });
				Options.EnableDocumentHostTestFixture = true;
				continue;
			}
			if (Arguments[Index] == "--document-host-reset-layout-test")
			{
				if (Options.EnableDocumentHostResetLayoutTest)
					return std::unexpected(EditorCommandLineError{
						EditorCommandLineErrorCode::DuplicateOption,
						"The --document-host-reset-layout-test option may be specified only once." });
				Options.EnableDocumentHostResetLayoutTest = true;
				continue;
			}
			if (Arguments[Index] == "--document-host-test-tab-tear-off")
			{
				if (Options.EnableDocumentHostTabTearOffTest)
					return std::unexpected(EditorCommandLineError{
						EditorCommandLineErrorCode::DuplicateOption,
						"The --document-host-test-tab-tear-off option may be specified only once." });
				Options.EnableDocumentHostTabTearOffTest = true;
				continue;
			}
			if (Arguments[Index] == "--asset-preview-stress")
			{
				if (Options.AssetPreviewStressManifest)
					return std::unexpected(EditorCommandLineError{
						EditorCommandLineErrorCode::DuplicateProjectOption,
						"The --asset-preview-stress option may be specified only once." });
				if (Index + 2 >= Arguments.size() || Arguments[Index + 1].starts_with("--") ||
					Arguments[Index + 2].starts_with("--"))
					return std::unexpected(EditorCommandLineError{
						EditorCommandLineErrorCode::MissingProjectPath,
						"The --asset-preview-stress option requires a manifest path and budget mode (entries or gpu)." });
				std::filesystem::path Manifest = std::filesystem::u8path(Arguments[++Index]);
				if (Manifest.is_relative())
					Manifest = WorkingDirectory / Manifest;
				std::error_code Error;
				Manifest = std::filesystem::absolute(Manifest, Error).lexically_normal();
				if (Error || !std::filesystem::is_regular_file(Manifest, Error) || Error)
					return std::unexpected(EditorCommandLineError{
						EditorCommandLineErrorCode::InvalidProjectPath,
						"The asset preview stress manifest is not an existing file." });
				Options.AssetPreviewStressManifest = std::move(Manifest);
				Options.AssetPreviewStressBudget = Arguments[++Index];
				if (Options.AssetPreviewStressBudget != "entries" && Options.AssetPreviewStressBudget != "gpu")
					return std::unexpected(EditorCommandLineError{
						EditorCommandLineErrorCode::UnexpectedArgument,
						"Asset preview stress budget must be 'entries' or 'gpu'." });
				continue;
			}
			if (Arguments[Index] != "--project")
				return std::unexpected(EditorCommandLineError{
					EditorCommandLineErrorCode::UnexpectedArgument,
					"Unexpected argument '" + std::string(Arguments[Index]) +
					"'. Supported options include --project <path>, --asset-preview-stress, "
					"the Debug-only --document-host-test-fixture, and the Debug-only "
					"--document-host-reset-layout-test and --document-host-test-tab-tear-off." });

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
		if (Options.EnableDocumentHostTestFixture && !Options.ProjectFile)
			return std::unexpected(EditorCommandLineError{
				EditorCommandLineErrorCode::MissingProjectPath,
				"The --document-host-test-fixture option requires --project <path>." });
		if (Options.EnableDocumentHostResetLayoutTest && !Options.EnableDocumentHostTestFixture)
			return std::unexpected(EditorCommandLineError{
				EditorCommandLineErrorCode::UnexpectedArgument,
				"The --document-host-reset-layout-test option requires --document-host-test-fixture." });
		if (Options.EnableDocumentHostTabTearOffTest && !Options.EnableDocumentHostTestFixture)
			return std::unexpected(EditorCommandLineError{
				EditorCommandLineErrorCode::UnexpectedArgument,
				"The --document-host-test-tab-tear-off option requires --document-host-test-fixture." });
		if (Options.EnableDocumentHostTabTearOffTest && Options.EnableDocumentHostResetLayoutTest)
			return std::unexpected(EditorCommandLineError{
				EditorCommandLineErrorCode::UnexpectedArgument,
				"The document tab tear-off and reset-layout fixture tests cannot run together." });

		return Options;
	}
}
