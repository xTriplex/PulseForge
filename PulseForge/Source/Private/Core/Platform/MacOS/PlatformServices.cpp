#include "Core/PlatformServices.h"
#include "Core/Platform/LogSink.h"

#include <algorithm>
#include <csignal>
#include <cstdint>
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

	PathResult GetUserDataDirectory()
	{
		const char* Home = std::getenv("HOME");
		if (!Home || Home[0] == '\0')
			return std::unexpected(PathError{ "HOME is unset; cannot resolve the user's application-support directory" });

		const std::filesystem::path HomeDirectory(Home);
		if (!HomeDirectory.is_absolute())
			return std::unexpected(PathError{ "HOME must be an absolute path to resolve the application-support directory" });
		return HomeDirectory / "Library" / "Application Support";
	}

	void DebugBreak() noexcept
	{
		(void)std::raise(SIGTRAP);
	}
} // namespace PulseForge::Platform
