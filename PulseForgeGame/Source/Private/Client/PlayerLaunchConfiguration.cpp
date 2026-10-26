#include "Client/PlayerLaunchConfiguration.h"

#include "Scene/UUID.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <limits>
#include <system_error>
#include <utility>

namespace PulseForgeGame
{
	namespace
	{
		PlayerLaunchConfigurationError MakeError(PlayerLaunchConfigurationErrorCode Code, std::string Message, size_t ArgumentIndex = 0)
		{
			return { Code, std::move(Message), ArgumentIndex };
		}

		std::filesystem::path MakeUtf8Path(std::string_view Value)
		{
			return std::filesystem::u8path(Value.begin(), Value.end());
		}

		bool ParsePositiveDimension(std::string_view Text, uint32_t& Value)
		{
			if (Text.empty()) return false;
			uint32_t Parsed = 0;
			const auto [End, Error] = std::from_chars(Text.data(), Text.data() + Text.size(), Parsed);
			if (Error != std::errc{} || End != Text.data() + Text.size() || Parsed == 0 ||
				Parsed > static_cast<uint32_t>(std::numeric_limits<int>::max()))
				return false;
			Value = Parsed;
			return true;
		}

		bool HasProjectExtension(const std::filesystem::path& Path)
		{
			std::string Extension = Path.extension().string();
			std::transform(Extension.begin(), Extension.end(), Extension.begin(), [](unsigned char Character)
			{
				return static_cast<char>(std::tolower(Character));
			});
			return Extension == ".pfproj";
		}
	} // namespace

	std::expected<PlayerLaunchConfiguration, PlayerLaunchConfigurationError> ParsePlayerLaunchConfiguration(
		std::span<const std::string_view> Arguments, const std::filesystem::path& InvocationDirectory)
	{
		std::error_code FileError;
		std::filesystem::path AbsoluteInvocationDirectory = std::filesystem::absolute(InvocationDirectory, FileError).lexically_normal();
		if (FileError || !std::filesystem::is_directory(AbsoluteInvocationDirectory, FileError) || FileError)
			return std::unexpected(MakeError(
				PlayerLaunchConfigurationErrorCode::InvalidWorkingDirectory, "The invocation directory must be an existing directory."));

		PlayerLaunchConfiguration Configuration;
		bool HasProject = false;
		bool HasCookedContent = false;
		bool HasStartupScene = false;
		bool HasRenderer = false;
		bool HasWindowSize = false;
		bool HasDiagnostics = false;

		for (size_t Index = 0; Index < Arguments.size(); ++Index)
		{
			const std::string_view Argument = Arguments[Index];
			if (Argument == "--project")
			{
				if (HasProject)
					return std::unexpected(MakeError(
						PlayerLaunchConfigurationErrorCode::DuplicateOption, "The --project option may be specified only once.", Index));
				if (Index + 1 >= Arguments.size() || Arguments[Index + 1].starts_with("--"))
					return std::unexpected(MakeError(
						PlayerLaunchConfigurationErrorCode::MissingOptionValue, "The --project option requires a .pfproj path.", Index));

				std::filesystem::path ProjectFile = MakeUtf8Path(Arguments[++Index]);
				if (ProjectFile.is_relative()) ProjectFile = AbsoluteInvocationDirectory / ProjectFile;
				ProjectFile = std::filesystem::absolute(ProjectFile, FileError).lexically_normal();
				if (FileError || !HasProjectExtension(ProjectFile) ||
					!std::filesystem::is_regular_file(ProjectFile, FileError) || FileError)
					return std::unexpected(MakeError(PlayerLaunchConfigurationErrorCode::InvalidProjectPath,
						"The project path must name an existing .pfproj file.", Index));

				Configuration.ProjectFile = std::move(ProjectFile);
				HasProject = true;
				continue;
			}

			if (Argument == "--cooked-content")
			{
				if (HasCookedContent)
					return std::unexpected(MakeError(PlayerLaunchConfigurationErrorCode::DuplicateOption,
						"The --cooked-content option may be specified only once.", Index));
				if (Index + 1 >= Arguments.size() || Arguments[Index + 1].starts_with("--"))
					return std::unexpected(MakeError(PlayerLaunchConfigurationErrorCode::MissingOptionValue,
						"The --cooked-content option requires a cooked content root.", Index));
				std::filesystem::path Root = MakeUtf8Path(Arguments[++Index]);
				if (Root.is_relative()) Root = AbsoluteInvocationDirectory / Root;
				Root = std::filesystem::absolute(Root, FileError).lexically_normal();
				if (FileError || !std::filesystem::is_directory(Root, FileError) || FileError)
					return std::unexpected(MakeError(PlayerLaunchConfigurationErrorCode::InvalidCookedContentPath,
						"The cooked content root must be an existing directory.", Index));
				Configuration.CookedContentRoot = std::move(Root);
				HasCookedContent = true;
				continue;
			}

			if (Argument == "--runtime-abi")
			{
				if (Index + 1 >= Arguments.size() || Arguments[Index + 1].starts_with("--"))
					return std::unexpected(MakeError(PlayerLaunchConfigurationErrorCode::MissingOptionValue,
						"The --runtime-abi option requires an ABI identifier.", Index));
				Configuration.RuntimeABI = std::string(Arguments[++Index]);
				if (Configuration.RuntimeABI.empty())
					return std::unexpected(MakeError(PlayerLaunchConfigurationErrorCode::UnexpectedArgument,
						"The runtime ABI identifier must not be empty.", Index));
				continue;
			}

			if (Argument == "--start-scene")
			{
				if (HasStartupScene)
					return std::unexpected(MakeError(PlayerLaunchConfigurationErrorCode::DuplicateOption,
						"The --start-scene option may be specified only once.", Index));
				if (Index + 1 >= Arguments.size() || Arguments[Index + 1].starts_with("--"))
					return std::unexpected(MakeError(
						PlayerLaunchConfigurationErrorCode::MissingOptionValue, "The --start-scene option requires a scene UUID.", Index));

				const auto SceneID = PulseForge::UUID::Parse(Arguments[++Index]);
				if (!SceneID || SceneID->IsNil())
					return std::unexpected(MakeError(
						PlayerLaunchConfigurationErrorCode::InvalidStartupScene, "The startup scene must be a non-nil UUID.", Index));
				Configuration.StartupScene = *SceneID;
				HasStartupScene = true;
				continue;
			}

			if (Argument == "--window-size")
			{
				if (HasWindowSize)
					return std::unexpected(MakeError(PlayerLaunchConfigurationErrorCode::DuplicateOption,
						"The --window-size option may be specified only once.", Index));
				if (Index + 1 >= Arguments.size() || Arguments[Index + 1].starts_with("--"))
					return std::unexpected(MakeError(PlayerLaunchConfigurationErrorCode::MissingOptionValue,
						"The --window-size option requires dimensions in WIDTHxHEIGHT form.", Index));

				const std::string_view Dimensions = Arguments[++Index];
				const size_t Separator = Dimensions.find('x');
				uint32_t Width = 0;
				uint32_t Height = 0;
				if (Separator == std::string_view::npos || Dimensions.find('x', Separator + 1) != std::string_view::npos ||
					!ParsePositiveDimension(Dimensions.substr(0, Separator), Width) ||
					!ParsePositiveDimension(Dimensions.substr(Separator + 1), Height))
					return std::unexpected(MakeError(PlayerLaunchConfigurationErrorCode::InvalidWindowSize,
						"Window dimensions must be positive integers in WIDTHxHEIGHT form.", Index));

				Configuration.WindowWidth = Width;
				Configuration.WindowHeight = Height;
				HasWindowSize = true;
				continue;
			}

			if (Argument == "--renderer")
			{
				if (HasRenderer)
					return std::unexpected(MakeError(
						PlayerLaunchConfigurationErrorCode::DuplicateOption, "The --renderer option may be specified only once.", Index));
				if (Index + 1 >= Arguments.size() || Arguments[Index + 1].starts_with("--"))
					return std::unexpected(MakeError(PlayerLaunchConfigurationErrorCode::MissingOptionValue,
						"The --renderer option requires 'vulkan' or 'opengl'.", Index));

				const std::string_view Value = Arguments[++Index];
				if (Value == "vulkan") Configuration.Renderer = PulseForge::RendererAPI::Vulkan;
				else if (Value == "opengl") Configuration.Renderer = PulseForge::RendererAPI::OpenGL;
				else
					return std::unexpected(MakeError(
						PlayerLaunchConfigurationErrorCode::InvalidRenderer, "The renderer must be 'vulkan' or 'opengl'.", Index));
				HasRenderer = true;
				continue;
			}

			if (Argument == "--diagnostics")
			{
				if (HasDiagnostics)
					return std::unexpected(MakeError(PlayerLaunchConfigurationErrorCode::DuplicateOption,
						"The --diagnostics option may be specified only once.", Index));
				Configuration.DiagnosticsEnabled = true;
				HasDiagnostics = true;
				continue;
			}

			return std::unexpected(MakeError(
				PlayerLaunchConfigurationErrorCode::UnexpectedArgument, "Unknown player launch argument: " + std::string(Argument), Index));
		}

		if (HasProject == HasCookedContent)
			return std::unexpected(MakeError(PlayerLaunchConfigurationErrorCode::MissingLaunchContent,
				"Specify exactly one of --project <path> or --cooked-content <root>."));

		return Configuration;
	}
} // namespace PulseForgeGame
