#include "Core/PlatformServices.h"
#include "Core/Platform/LogSink.h"

#include <algorithm>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <mach-o/dyld.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace PulseForge::Platform
{
	namespace Detail
	{
		std::shared_ptr<spdlog::sinks::sink> CreateLogSink()
		{
			return std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
		}
	} // namespace Detail

	PathResult GetExecutablePath()
	{
		constexpr size_t MaximumPathLength = 1024 * 1024;
		std::vector<char> Buffer(256);
		while (Buffer.size() <= MaximumPathLength)
		{
			uint32_t BufferSize = static_cast<uint32_t>(Buffer.size());
			if (_NSGetExecutablePath(Buffer.data(), &BufferSize) == 0)
				return std::filesystem::path(Buffer.data());

			if (BufferSize > Buffer.size() && BufferSize <= MaximumPathLength)
				Buffer.resize(BufferSize);
			else if (Buffer.size() < MaximumPathLength)
				Buffer.resize((std::min)(Buffer.size() * 2, MaximumPathLength));
			else
				break;
		}

		return std::unexpected(PathError{ "The executable path reported by dyld exceeds 1 MiB" });
	}

	PathResult GetResourceDirectory()
	{
		auto ExecutablePath = GetExecutablePath();
		if (!ExecutablePath) return std::unexpected(ExecutablePath.error());
		if (!ExecutablePath->is_absolute() || ExecutablePath->filename().empty())
			return std::unexpected(PathError{ "The executable path must be absolute and name a file to resolve application resources" });

		const std::filesystem::path ExecutableDirectory = ExecutablePath->parent_path();
		if (ExecutableDirectory.empty())
			return std::unexpected(PathError{ "Could not resolve the executable directory for application resources" });

		const std::filesystem::path ContentsDirectory = ExecutableDirectory.parent_path();
		const std::filesystem::path BundleDirectory = ContentsDirectory.parent_path();
		const bool IsAppBundle = ExecutableDirectory.filename() == "MacOS" && ContentsDirectory.filename() == "Contents" &&
								 BundleDirectory.extension() == ".app";
		const std::filesystem::path ResourceDirectory = IsAppBundle ? ContentsDirectory / "Resources" : ExecutableDirectory;

		std::error_code Error;
		if (!std::filesystem::is_directory(ResourceDirectory, Error) || Error)
		{
			const std::string Reason = Error ? Error.message() : "the path is missing or is not a directory";
			return std::unexpected(
				PathError{ "Could not access the application resource directory '" + ResourceDirectory.string() + "': " + Reason });
		}

		return ResourceDirectory;
	}

	PathResult GetUserDataDirectory()
	{
		if (const char* Override = std::getenv("PULSEFORGE_USER_DATA_DIR"); Override && Override[0] != '\0')
		{
			const std::filesystem::path Directory = std::filesystem::u8path(Override);
			if (!Directory.is_absolute())
				return std::unexpected(PathError{ "PULSEFORGE_USER_DATA_DIR must be an absolute path" });
			return Directory;
		}

		const char* Home = std::getenv("HOME");
		if (!Home || Home[0] == '\0')
			return std::unexpected(PathError{ "HOME is unset; cannot resolve the user's application-support directory" });

		const std::filesystem::path HomeDirectory(Home);
		if (!HomeDirectory.is_absolute())
			return std::unexpected(PathError{ "HOME must be an absolute path to resolve the application-support directory" });
		return HomeDirectory / "Library" / "Application Support";
	}

	std::expected<void, PathError> ReplaceFileAtomically(
		const std::filesystem::path& TemporaryPath,
		const std::filesystem::path& DestinationPath)
	{
		if (rename(TemporaryPath.c_str(), DestinationPath.c_str()) != 0)
			return std::unexpected(PathError{ "Could not replace the settings file: " + std::string(std::strerror(errno)) });
		return {};
	}

	void DebugBreak() noexcept
	{
		(void)std::raise(SIGTRAP);
	}
} // namespace PulseForge::Platform
