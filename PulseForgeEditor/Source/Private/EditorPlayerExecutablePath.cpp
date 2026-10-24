#include "Editor/EditorPlayerExecutablePath.h"

#if (defined(PF_PLATFORM_WINDOWS) + defined(PF_PLATFORM_LINUX) + defined(PF_PLATFORM_MACOS)) != 1
#error "PulseForgeEditor requires exactly one supported PF_PLATFORM_* definition."
#endif

namespace PulseForgeEditor
{
	EditorPlayerPlatform GetEditorPlayerPlatform() noexcept
	{
#if defined(PF_PLATFORM_WINDOWS)
		return EditorPlayerPlatform::Windows;
#elif defined(PF_PLATFORM_LINUX)
		return EditorPlayerPlatform::Linux;
#else
		return EditorPlayerPlatform::MacOS;
#endif
	}

	std::expected<std::filesystem::path, std::string> ResolveEditorPlayerExecutablePath(
		const std::filesystem::path& EditorExecutable,
		EditorPlayerPlatform Platform)
	{
		if (EditorExecutable.empty() || EditorExecutable.parent_path().empty())
			return std::unexpected("The editor executable path has no parent directory.");

		switch (Platform)
		{
		case EditorPlayerPlatform::Windows:
			return EditorExecutable.parent_path() / "PulseForgeGame.exe";
		case EditorPlayerPlatform::Linux:
			return EditorExecutable.parent_path() / "PulseForgeGame";
		case EditorPlayerPlatform::MacOS:
			return EditorExecutable.parent_path().parent_path().parent_path().parent_path() / "PulseForgeGame.app" /
				   "Contents" / "MacOS" / "PulseForgeGame";
		default:
			return std::unexpected("The editor player path is unsupported for this platform.");
		}
	}
} // namespace PulseForgeEditor
