#include "TestSupport.h"

#include "Client/PlayerLaunchConfiguration.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

namespace
{
	std::string PathToUtf8(const std::filesystem::path& Path)
	{
		const std::u8string Encoded = Path.u8string();
		return std::string(reinterpret_cast<const char*>(Encoded.data()), Encoded.size());
	}

	void TestPlayerLaunchConfiguration(TestRunner& Tests)
	{
		using namespace PulseForgeGame;
		const std::filesystem::path TemporaryDirectory = std::filesystem::temp_directory_path();
		const auto Identifier = PulseForge::UUID::Generate();
		PF_CHECK(Tests, Identifier.has_value());
		if (!Identifier) return;

		const std::string IdentifierText = Identifier->ToString();
		const std::u8string WideIdentifier(IdentifierText.begin(), IdentifierText.end());
		const std::filesystem::path Root =
			TemporaryDirectory / std::filesystem::u8path(u8"PulseForge Player Config 测试 " + WideIdentifier);
		std::error_code FileError;
		std::filesystem::create_directories(Root, FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError) return;
		struct DirectoryCleanup
		{
			std::filesystem::path Path;
			~DirectoryCleanup()
			{
				std::error_code Error;
				std::filesystem::remove_all(Path, Error);
			}
		} Cleanup{ Root };

		const std::filesystem::path ProjectFile = Root / std::filesystem::u8path(u8"项目 With Spaces.pfproj");
		{
			std::ofstream Project(ProjectFile, std::ios::binary | std::ios::trunc);
			Project << "fixture";
			PF_CHECK(Tests, static_cast<bool>(Project));
		}
		const std::string ProjectPath = PathToUtf8(ProjectFile.filename());

		const std::span<const std::string_view> NoArguments;
		const auto MissingProject = ParsePlayerLaunchConfiguration(NoArguments, Root);
		PF_CHECK(Tests, !MissingProject);
		PF_CHECK(Tests, !MissingProject && MissingProject.error().Code == PlayerLaunchConfigurationErrorCode::MissingProjectPath);

		const std::array BasicArguments = { std::string_view("--project"), std::string_view(ProjectPath) };
		const auto Basic = ParsePlayerLaunchConfiguration(BasicArguments, Root);
		PF_CHECK(Tests, Basic.has_value());
		if (Basic)
		{
			PF_CHECK(Tests, Basic->ProjectFile == std::filesystem::absolute(ProjectFile).lexically_normal());
			PF_CHECK(Tests, !Basic->StartupScene && !Basic->WindowWidth && !Basic->WindowHeight && !Basic->Renderer);
			PF_CHECK(Tests, !Basic->DiagnosticsEnabled);
		}

		const std::array FullArguments = { std::string_view("--project"), std::string_view(ProjectPath), std::string_view("--start-scene"),
			std::string_view("01234567-89ab-cdef-0123-456789abcdef"), std::string_view("--window-size"), std::string_view("1920x1080"),
			std::string_view("--renderer"), std::string_view("vulkan"), std::string_view("--diagnostics") };
		const auto Full = ParsePlayerLaunchConfiguration(FullArguments, Root);
		PF_CHECK(Tests, Full.has_value());
		if (Full)
		{
			PF_CHECK(Tests, Full->StartupScene.has_value());
			PF_CHECK(Tests, Full->StartupScene && Full->StartupScene->ToString() == "01234567-89ab-cdef-0123-456789abcdef");
			PF_CHECK(Tests, Full->WindowWidth == 1920 && Full->WindowHeight == 1080);
			PF_CHECK(Tests, Full->Renderer == PulseForge::RendererAPI::Vulkan);
			PF_CHECK(Tests, Full->DiagnosticsEnabled);
		}

		const std::array OpenGLArguments = { std::string_view("--project"), std::string_view(ProjectPath), std::string_view("--renderer"),
			std::string_view("opengl") };
		const auto OpenGL = ParsePlayerLaunchConfiguration(OpenGLArguments, Root);
		PF_CHECK(Tests, OpenGL && OpenGL->Renderer == PulseForge::RendererAPI::OpenGL);

		const std::string AbsoluteProjectPath = PathToUtf8(ProjectFile);
		const std::array AbsoluteArguments = { std::string_view("--project"), std::string_view(AbsoluteProjectPath) };
		const auto Absolute = ParsePlayerLaunchConfiguration(AbsoluteArguments, TemporaryDirectory);
		PF_CHECK(Tests, Absolute.has_value());
		PF_CHECK(Tests, Absolute && Absolute->ProjectFile == std::filesystem::absolute(ProjectFile).lexically_normal());

		const std::array MissingProjectValue = { std::string_view("--project") };
		const auto MissingValue = ParsePlayerLaunchConfiguration(MissingProjectValue, Root);
		PF_CHECK(Tests, !MissingValue && MissingValue.error().Code == PlayerLaunchConfigurationErrorCode::MissingOptionValue);

		const std::array DuplicateProject = { std::string_view("--project"), std::string_view(ProjectPath), std::string_view("--project"),
			std::string_view(ProjectPath) };
		const auto DuplicateProjectResult = ParsePlayerLaunchConfiguration(DuplicateProject, Root);
		PF_CHECK(
			Tests, !DuplicateProjectResult && DuplicateProjectResult.error().Code == PlayerLaunchConfigurationErrorCode::DuplicateOption);

		const std::array DuplicateWindow = { std::string_view("--project"), std::string_view(ProjectPath),
			std::string_view("--window-size"), std::string_view("640x480"), std::string_view("--window-size"),
			std::string_view("800x600") };
		const auto DuplicateWindowResult = ParsePlayerLaunchConfiguration(DuplicateWindow, Root);
		PF_CHECK(
			Tests, !DuplicateWindowResult && DuplicateWindowResult.error().Code == PlayerLaunchConfigurationErrorCode::DuplicateOption);

		const std::array DuplicateDiagnostics = { std::string_view("--project"), std::string_view(ProjectPath),
			std::string_view("--diagnostics"), std::string_view("--diagnostics") };
		const auto DuplicateDiagnosticsResult = ParsePlayerLaunchConfiguration(DuplicateDiagnostics, Root);
		PF_CHECK(Tests,
			!DuplicateDiagnosticsResult && DuplicateDiagnosticsResult.error().Code == PlayerLaunchConfigurationErrorCode::DuplicateOption);

		const std::array DuplicateRenderer = { std::string_view("--project"), std::string_view(ProjectPath), std::string_view("--renderer"),
			std::string_view("vulkan"), std::string_view("--renderer"), std::string_view("opengl") };
		const auto DuplicateRendererResult = ParsePlayerLaunchConfiguration(DuplicateRenderer, Root);
		PF_CHECK(
			Tests, !DuplicateRendererResult && DuplicateRendererResult.error().Code == PlayerLaunchConfigurationErrorCode::DuplicateOption);

		const std::array InvalidRenderer = { std::string_view("--project"), std::string_view(ProjectPath), std::string_view("--renderer"),
			std::string_view("directx") };
		const auto InvalidRendererResult = ParsePlayerLaunchConfiguration(InvalidRenderer, Root);
		PF_CHECK(
			Tests, !InvalidRendererResult && InvalidRendererResult.error().Code == PlayerLaunchConfigurationErrorCode::InvalidRenderer);

		const std::array MissingRenderer = { std::string_view("--project"), std::string_view(ProjectPath), std::string_view("--renderer") };
		const auto MissingRendererResult = ParsePlayerLaunchConfiguration(MissingRenderer, Root);
		PF_CHECK(
			Tests, !MissingRendererResult && MissingRendererResult.error().Code == PlayerLaunchConfigurationErrorCode::MissingOptionValue);

		const std::array UnknownArgument = { std::string_view("--project"), std::string_view(ProjectPath), std::string_view("--sample") };
		const auto UnknownResult = ParsePlayerLaunchConfiguration(UnknownArgument, Root);
		PF_CHECK(Tests, !UnknownResult && UnknownResult.error().Code == PlayerLaunchConfigurationErrorCode::UnexpectedArgument);

		const std::array InvalidScene = { std::string_view("--project"), std::string_view(ProjectPath), std::string_view("--start-scene"),
			std::string_view("not-a-uuid") };
		const auto InvalidSceneResult = ParsePlayerLaunchConfiguration(InvalidScene, Root);
		PF_CHECK(Tests, !InvalidSceneResult && InvalidSceneResult.error().Code == PlayerLaunchConfigurationErrorCode::InvalidStartupScene);

		const std::array NilScene = { std::string_view("--project"), std::string_view(ProjectPath), std::string_view("--start-scene"),
			std::string_view("00000000-0000-0000-0000-000000000000") };
		const auto NilSceneResult = ParsePlayerLaunchConfiguration(NilScene, Root);
		PF_CHECK(Tests, !NilSceneResult && NilSceneResult.error().Code == PlayerLaunchConfigurationErrorCode::InvalidStartupScene);

		for (const std::string_view Size : { "0x600", "640x0", "640", "640x480x2", "640x-1", "4294967295x480" })
		{
			const std::array Arguments = { std::string_view("--project"), std::string_view(ProjectPath), std::string_view("--window-size"),
				Size };
			const auto Result = ParsePlayerLaunchConfiguration(Arguments, Root);
			PF_CHECK(Tests, !Result && Result.error().Code == PlayerLaunchConfigurationErrorCode::InvalidWindowSize);
		}

		const std::array MissingWindowValue = { std::string_view("--project"), std::string_view(ProjectPath),
			std::string_view("--window-size") };
		const auto MissingWindow = ParsePlayerLaunchConfiguration(MissingWindowValue, Root);
		PF_CHECK(Tests, !MissingWindow && MissingWindow.error().Code == PlayerLaunchConfigurationErrorCode::MissingOptionValue);

		const std::array InvalidExtension = { std::string_view("--project"), std::string_view("fixture.txt") };
		const auto InvalidExtensionResult = ParsePlayerLaunchConfiguration(InvalidExtension, Root);
		PF_CHECK(Tests,
			!InvalidExtensionResult && InvalidExtensionResult.error().Code == PlayerLaunchConfigurationErrorCode::InvalidProjectPath);

		const std::array MissingFile = { std::string_view("--project"), std::string_view("missing.pfproj") };
		const auto MissingFileResult = ParsePlayerLaunchConfiguration(MissingFile, Root);
		PF_CHECK(Tests, !MissingFileResult && MissingFileResult.error().Code == PlayerLaunchConfigurationErrorCode::InvalidProjectPath);
	}
} // namespace

void RunPlayerLaunchConfigurationTests(TestRunner& Tests)
{
	TestPlayerLaunchConfiguration(Tests);
}
