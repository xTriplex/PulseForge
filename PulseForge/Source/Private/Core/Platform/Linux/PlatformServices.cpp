#include "Core/PlatformServices.h"
#include "Core/Platform/LogSink.h"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <spdlog/sinks/stdout_color_sinks.h>

#include <unistd.h>

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
			errno = 0;
			const ssize_t Length = readlink("/proc/self/exe", Buffer.data(), Buffer.size());
			if (Length < 0)
			{
				const int Error = errno;
				return std::unexpected(PathError{ "Could not read /proc/self/exe: " + std::string(std::strerror(Error)) });
			}
			if (static_cast<size_t>(Length) < Buffer.size())
				return std::filesystem::path(std::string(Buffer.data(), static_cast<size_t>(Length)));
			if (Buffer.size() == MaximumPathLength) break;
			Buffer.resize((std::min)(Buffer.size() * 2, MaximumPathLength));
		}

		return std::unexpected(PathError{ "The executable path reported by /proc/self/exe exceeds 1 MiB" });
	}

	PathResult GetResourceDirectory()
	{
		auto ExecutablePath = GetExecutablePath();
		if (!ExecutablePath) return std::unexpected(ExecutablePath.error());

		const std::filesystem::path ResourceDirectory = ExecutablePath->parent_path();
		std::error_code Error;
		if (ResourceDirectory.empty() || !std::filesystem::is_directory(ResourceDirectory, Error) || Error)
			return std::unexpected(PathError{ "Could not resolve the executable's resource directory" });
		return ResourceDirectory;
	}

	PathResult GetUserDataDirectory()
	{
		if (const char* XdgDataHome = std::getenv("XDG_DATA_HOME"); XdgDataHome && XdgDataHome[0] != '\0')
		{
			const std::filesystem::path Directory(XdgDataHome);
			if (!Directory.is_absolute()) return std::unexpected(PathError{ "XDG_DATA_HOME must be an absolute path when set" });
			return Directory;
		}

		const char* Home = std::getenv("HOME");
		if (!Home || Home[0] == '\0') return std::unexpected(PathError{ "HOME is unset; cannot resolve the user's data directory" });

		const std::filesystem::path HomeDirectory(Home);
		if (!HomeDirectory.is_absolute())
			return std::unexpected(PathError{ "HOME must be an absolute path to resolve the user's data directory" });
		return HomeDirectory / ".local" / "share";
	}

	void DebugBreak() noexcept
	{
		(void)std::raise(SIGTRAP);
	}
} // namespace PulseForge::Platform
