#include "Editor/EditorCommandLine.h"

#include "Assets/AssetTypes.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <system_error>
#include <utility>

namespace PulseForgeEditor
{
	[[nodiscard]] std::expected<EditorCommandLineOptions, EditorCommandLineError> ParseEditorCommandLine(
		std::span<const std::string_view> Arguments, const std::filesystem::path& WorkingDirectory)
	{
		EditorCommandLineOptions Options;
		for (std::size_t Index = 0; Index < Arguments.size(); ++Index)
		{
			if (Arguments[Index] == "--document-host-test-fixture")
			{
				if (Options.EnableDocumentHostTestFixture)
					return std::unexpected(EditorCommandLineError{ EditorCommandLineErrorCode::DuplicateOption,
						"The --document-host-test-fixture option may be specified only once." });
				Options.EnableDocumentHostTestFixture = true;
				continue;
			}
			if (Arguments[Index] == "--document-host-reset-layout-test")
			{
				if (Options.EnableDocumentHostResetLayoutTest)
					return std::unexpected(EditorCommandLineError{ EditorCommandLineErrorCode::DuplicateOption,
						"The --document-host-reset-layout-test option may be specified only once." });
				Options.EnableDocumentHostResetLayoutTest = true;
				continue;
			}
			if (Arguments[Index] == "--document-host-test-tab-tear-off")
			{
				if (Options.EnableDocumentHostTabTearOffTest)
					return std::unexpected(EditorCommandLineError{ EditorCommandLineErrorCode::DuplicateOption,
						"The --document-host-test-tab-tear-off option may be specified only once." });
				Options.EnableDocumentHostTabTearOffTest = true;
				continue;
			}
			if (Arguments[Index] == "--document-host-test-tab-reattach")
			{
				if (Options.EnableDocumentHostTabReattachTest)
					return std::unexpected(EditorCommandLineError{ EditorCommandLineErrorCode::DuplicateOption,
						"The --document-host-test-tab-reattach option may be specified only once." });
				Options.EnableDocumentHostTabReattachTest = true;
				continue;
			}
			if (Arguments[Index] == "--document-host-test-tab-reorder")
			{
				if (Options.EnableDocumentHostTabReorderTest)
					return std::unexpected(EditorCommandLineError{ EditorCommandLineErrorCode::DuplicateOption,
						"The --document-host-test-tab-reorder option may be specified only once." });
				Options.EnableDocumentHostTabReorderTest = true;
				continue;
			}
			if (Arguments[Index] == "--document-workspace-test-switch")
			{
				if (Options.EnableDocumentWorkspaceSwitchTest)
					return std::unexpected(EditorCommandLineError{ EditorCommandLineErrorCode::DuplicateOption,
						"The --document-workspace-test-switch option may be specified only once." });
				Options.EnableDocumentWorkspaceSwitchTest = true;
				continue;
			}
			if (Arguments[Index] == "--document-host-test-presentation-failure")
			{
				if (Options.EnableDocumentHostPresentationFailureTest)
					return std::unexpected(EditorCommandLineError{ EditorCommandLineErrorCode::DuplicateOption,
						"The --document-host-test-presentation-failure option may be specified only once." });
				Options.EnableDocumentHostPresentationFailureTest = true;
				continue;
			}
			if (Arguments[Index] == "--document-host-test-acquire-out-of-date")
			{
				if (Options.EnableDocumentHostAcquireOutOfDateTest)
					return std::unexpected(EditorCommandLineError{ EditorCommandLineErrorCode::DuplicateOption,
						"The --document-host-test-acquire-out-of-date option may be specified only once." });
				Options.EnableDocumentHostAcquireOutOfDateTest = true;
				continue;
			}
			if (Arguments[Index] == "--document-host-test-image-allocation-failure")
			{
				if (Options.EnableDocumentHostImageAllocationFailureTest)
					return std::unexpected(EditorCommandLineError{ EditorCommandLineErrorCode::DuplicateOption,
						"The --document-host-test-image-allocation-failure option may be specified only once." });
				Options.EnableDocumentHostImageAllocationFailureTest = true;
				continue;
			}
			if (Arguments[Index] == "--asset-preview-stress")
			{
				if (Options.AssetPreviewStressManifest)
					return std::unexpected(EditorCommandLineError{ EditorCommandLineErrorCode::DuplicateProjectOption,
						"The --asset-preview-stress option may be specified only once." });
				if (Index + 2 >= Arguments.size() || Arguments[Index + 1].starts_with("--") || Arguments[Index + 2].starts_with("--"))
					return std::unexpected(EditorCommandLineError{ EditorCommandLineErrorCode::MissingProjectPath,
						"The --asset-preview-stress option requires a manifest path and budget mode (entries or gpu)." });
				std::filesystem::path Manifest = std::filesystem::u8path(Arguments[++Index]);
				if (Manifest.is_relative()) Manifest = WorkingDirectory / Manifest;
				std::error_code Error;
				Manifest = std::filesystem::absolute(Manifest, Error).lexically_normal();
				if (Error || !std::filesystem::is_regular_file(Manifest, Error) || Error)
					return std::unexpected(EditorCommandLineError{
						EditorCommandLineErrorCode::InvalidProjectPath, "The asset preview stress manifest is not an existing file." });
				Options.AssetPreviewStressManifest = std::move(Manifest);
				Options.AssetPreviewStressBudget = Arguments[++Index];
				if (Options.AssetPreviewStressBudget != "entries" && Options.AssetPreviewStressBudget != "gpu")
					return std::unexpected(EditorCommandLineError{
						EditorCommandLineErrorCode::UnexpectedArgument, "Asset preview stress budget must be 'entries' or 'gpu'." });
				continue;
			}
			if (Arguments[Index] == "--project-open-render-test")
			{
				if (Options.ProjectOpenRenderTestFile)
					return std::unexpected(EditorCommandLineError{
						EditorCommandLineErrorCode::DuplicateOption, "The --project-open-render-test option may be specified only once." });
				if (Index + 1 >= Arguments.size() || Arguments[Index + 1].starts_with("--"))
					return std::unexpected(EditorCommandLineError{
						EditorCommandLineErrorCode::MissingProjectPath, "The --project-open-render-test option requires a .pfproj path." });
				std::filesystem::path ProjectFile = std::filesystem::u8path(Arguments[++Index]);
				if (ProjectFile.is_relative()) ProjectFile = WorkingDirectory / ProjectFile;
				std::error_code Error;
				ProjectFile = std::filesystem::absolute(ProjectFile, Error).lexically_normal();
				if (Error || !PulseForge::HasAssetTypeExtension(ProjectFile, PulseForge::AssetFileType::Project) ||
					!std::filesystem::is_regular_file(ProjectFile, Error) || Error)
					return std::unexpected(EditorCommandLineError{ EditorCommandLineErrorCode::InvalidProjectPath,
						"The project-open render test requires an existing .pfproj file." });
				Options.ProjectOpenRenderTestFile = std::move(ProjectFile);
				continue;
			}
			if (Arguments[Index] != "--project")
				return std::unexpected(EditorCommandLineError{ EditorCommandLineErrorCode::UnexpectedArgument,
					"Unexpected argument '" + std::string(Arguments[Index]) +
						"'. Supported options include --project <path>, --asset-preview-stress, "
						"and the Debug-only --project-open-render-test <path>, "
						"the Debug-only --document-host-test-fixture, and the Debug-only "
						"--document-host-reset-layout-test, --document-host-test-tab-tear-off, "
						"--document-host-test-tab-reattach, --document-host-test-tab-reorder, and "
						"--document-workspace-test-switch, and "
						"--document-host-test-presentation-failure, --document-host-test-acquire-out-of-date, "
						"and --document-host-test-image-allocation-failure." });

			if (Options.ProjectFile)
				return std::unexpected(EditorCommandLineError{
					EditorCommandLineErrorCode::DuplicateProjectOption, "The --project option may be specified only once." });

			if (Index + 1 >= Arguments.size() || Arguments[Index + 1].starts_with("--"))
				return std::unexpected(EditorCommandLineError{
					EditorCommandLineErrorCode::MissingProjectPath, "The --project option requires a .pfproj path." });

			const std::string_view SuppliedPath = Arguments[++Index];
			std::error_code Error;
			std::filesystem::path ProjectFile = std::filesystem::u8path(SuppliedPath);
			if (ProjectFile.is_relative()) ProjectFile = WorkingDirectory / ProjectFile;
			ProjectFile = std::filesystem::absolute(ProjectFile, Error).lexically_normal();
			if (Error)
				return std::unexpected(EditorCommandLineError{
					EditorCommandLineErrorCode::InvalidProjectPath, "The project path could not be resolved: " + Error.message() });

			if (!PulseForge::HasAssetTypeExtension(ProjectFile, PulseForge::AssetFileType::Project) ||
				!std::filesystem::is_regular_file(ProjectFile, Error) || Error)
			{
				std::string Message = "The project path is not an existing .pfproj file: ";
				Message += ProjectFile.string();
				return std::unexpected(EditorCommandLineError{ EditorCommandLineErrorCode::InvalidProjectPath, std::move(Message) });
			}

			Options.ProjectFile = std::move(ProjectFile);
		}
		if (Options.EnableDocumentHostTestFixture && !Options.ProjectFile)
			return std::unexpected(EditorCommandLineError{
				EditorCommandLineErrorCode::MissingProjectPath, "The --document-host-test-fixture option requires --project <path>." });
		if (Options.EnableDocumentWorkspaceSwitchTest && !Options.EnableDocumentHostTestFixture)
			return std::unexpected(EditorCommandLineError{ EditorCommandLineErrorCode::UnexpectedArgument,
				"The --document-workspace-test-switch option requires --document-host-test-fixture." });
		if (Options.EnableDocumentHostResetLayoutTest && !Options.EnableDocumentHostTestFixture)
			return std::unexpected(EditorCommandLineError{ EditorCommandLineErrorCode::UnexpectedArgument,
				"The --document-host-reset-layout-test option requires --document-host-test-fixture." });
		if (Options.EnableDocumentHostTabTearOffTest && !Options.EnableDocumentHostTestFixture)
			return std::unexpected(EditorCommandLineError{ EditorCommandLineErrorCode::UnexpectedArgument,
				"The --document-host-test-tab-tear-off option requires --document-host-test-fixture." });
		if (Options.EnableDocumentHostTabReattachTest && !Options.EnableDocumentHostTestFixture)
			return std::unexpected(EditorCommandLineError{ EditorCommandLineErrorCode::UnexpectedArgument,
				"The --document-host-test-tab-reattach option requires --document-host-test-fixture." });
		if (Options.EnableDocumentHostTabReorderTest && !Options.EnableDocumentHostTestFixture)
			return std::unexpected(EditorCommandLineError{ EditorCommandLineErrorCode::UnexpectedArgument,
				"The --document-host-test-tab-reorder option requires --document-host-test-fixture." });
		if (Options.EnableDocumentHostPresentationFailureTest && !Options.EnableDocumentHostTestFixture)
			return std::unexpected(EditorCommandLineError{ EditorCommandLineErrorCode::UnexpectedArgument,
				"The --document-host-test-presentation-failure option requires --document-host-test-fixture." });
		if (Options.EnableDocumentHostAcquireOutOfDateTest && !Options.EnableDocumentHostTestFixture)
			return std::unexpected(EditorCommandLineError{ EditorCommandLineErrorCode::UnexpectedArgument,
				"The --document-host-test-acquire-out-of-date option requires --document-host-test-fixture." });
		if (Options.EnableDocumentHostImageAllocationFailureTest && !Options.EnableDocumentHostTestFixture)
			return std::unexpected(EditorCommandLineError{ EditorCommandLineErrorCode::UnexpectedArgument,
				"The --document-host-test-image-allocation-failure option requires --document-host-test-fixture." });
		if (Options.EnableDocumentHostTabTearOffTest && Options.EnableDocumentHostResetLayoutTest)
			return std::unexpected(EditorCommandLineError{ EditorCommandLineErrorCode::UnexpectedArgument,
				"The document tab tear-off and reset-layout fixture tests cannot run together." });
		if (Options.EnableDocumentHostTabReattachTest &&
			(Options.EnableDocumentHostTabTearOffTest || Options.EnableDocumentHostResetLayoutTest ||
				Options.EnableDocumentHostTabReorderTest))
			return std::unexpected(EditorCommandLineError{ EditorCommandLineErrorCode::UnexpectedArgument,
				"The document tab reattachment fixture cannot run with another document-host UI fixture." });
		if (Options.EnableDocumentHostTabReorderTest &&
			(Options.EnableDocumentHostTabTearOffTest || Options.EnableDocumentHostResetLayoutTest))
			return std::unexpected(EditorCommandLineError{ EditorCommandLineErrorCode::UnexpectedArgument,
				"The document tab reorder fixture cannot run with another document-host UI fixture." });
		if (Options.EnableDocumentWorkspaceSwitchTest &&
			(Options.EnableDocumentHostTabTearOffTest || Options.EnableDocumentHostTabReattachTest ||
				Options.EnableDocumentHostTabReorderTest || Options.EnableDocumentHostResetLayoutTest ||
				Options.EnableDocumentHostPresentationFailureTest || Options.EnableDocumentHostAcquireOutOfDateTest ||
				Options.EnableDocumentHostImageAllocationFailureTest))
			return std::unexpected(EditorCommandLineError{ EditorCommandLineErrorCode::UnexpectedArgument,
				"The document workspace switch fixture cannot run with another document-host test fixture." });
		const uint32_t FailureFixtureCount = static_cast<uint32_t>(Options.EnableDocumentHostPresentationFailureTest) +
											 static_cast<uint32_t>(Options.EnableDocumentHostAcquireOutOfDateTest) +
											 static_cast<uint32_t>(Options.EnableDocumentHostImageAllocationFailureTest);
		if (FailureFixtureCount > 1 ||
			(FailureFixtureCount != 0 && (Options.EnableDocumentHostTabTearOffTest || Options.EnableDocumentHostTabReattachTest ||
											 Options.EnableDocumentHostTabReorderTest || Options.EnableDocumentHostResetLayoutTest)))
			return std::unexpected(EditorCommandLineError{ EditorCommandLineErrorCode::UnexpectedArgument,
				"Native presentation failure fixtures cannot run with each other or another document-host UI fixture." });

		return Options;
	}
} // namespace PulseForgeEditor
