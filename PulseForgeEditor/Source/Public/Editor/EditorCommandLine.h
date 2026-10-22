#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

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
		std::optional<std::filesystem::path> ProjectOpenRenderTestFile;
		std::optional<std::filesystem::path> AssetPreviewStressManifest;
		std::optional<std::filesystem::path> DiagnosticsCaptureSmokeOutput;
		std::optional<std::string> DocumentHostCloseUiTestScenario;
		std::string AssetPreviewStressBudget;
		bool EnableEditorRuntimePlayStopTest = false;
		bool EnableSceneViewportNativeSmoke = false;
		bool EnableDocumentHostTestFixture = false;
		bool EnableDocumentHostResetLayoutTest = false;
		bool EnableDocumentHostTabTearOffTest = false;
		bool EnableDocumentHostTabReattachTest = false;
		bool EnableDocumentHostTabReorderTest = false;
		bool EnableDocumentWorkspaceSwitchTest = false;
		bool EnableDocumentHostPresentationFailureTest = false;
		bool EnableDocumentHostAcquireOutOfDateTest = false;
		bool EnableDocumentHostImageAllocationFailureTest = false;
	};

	[[nodiscard]] std::expected<EditorCommandLineOptions, EditorCommandLineError> ParseEditorCommandLine(
		std::span<const std::string_view> Arguments, const std::filesystem::path& WorkingDirectory);
} // namespace PulseForgeEditor
