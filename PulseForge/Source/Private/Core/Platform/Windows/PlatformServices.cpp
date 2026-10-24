#include "Core/PlatformServices.h"
#include "Core/Platform/LogSink.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <Objbase.h>
#include <ShlObj.h>

#include <spdlog/sinks/msvc_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace PulseForge::Platform
{
	namespace Detail
	{
		std::shared_ptr<spdlog::sinks::sink> CreateLogSink()
		{
			const HANDLE StandardOutput = GetStdHandle(STD_OUTPUT_HANDLE);
			if (GetConsoleWindow() || (StandardOutput && StandardOutput != INVALID_HANDLE_VALUE))
				return std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
			return std::make_shared<spdlog::sinks::msvc_sink_mt>(false);
		}
	} // namespace Detail

	namespace
	{
		struct KnownFolderDeleter
		{
			void operator()(wchar_t* Path) const noexcept
			{
				if (Path) CoTaskMemFree(Path);
			}
		};
	} // namespace

	PathResult GetExecutablePath()
	{
		constexpr DWORD MaximumPathLength = 32768;
		std::vector<wchar_t> Buffer(512);
		while (Buffer.size() <= MaximumPathLength)
		{
			SetLastError(ERROR_SUCCESS);
			const DWORD Length = GetModuleFileNameW(nullptr, Buffer.data(), static_cast<DWORD>(Buffer.size()));
			if (Length == 0)
				return std::unexpected(
					PathError{ "Windows could not locate the current executable (error " + std::to_string(GetLastError()) + ")" });
			if (Length < Buffer.size() - 1) return std::filesystem::path(std::wstring(Buffer.data(), Length));
			if (Buffer.size() == MaximumPathLength) break;
			Buffer.resize((std::min)(Buffer.size() * 2, static_cast<size_t>(MaximumPathLength)));
		}

		return std::unexpected(PathError{ "The current executable path exceeds the Windows path limit" });
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
		PWSTR RawPath = nullptr;
		const HRESULT Result = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &RawPath);
		std::unique_ptr<wchar_t, KnownFolderDeleter> Path(RawPath);
		if (FAILED(Result) || !Path)
			return std::unexpected(PathError{ "Windows could not resolve the user's Local AppData directory (HRESULT " +
											  std::to_string(static_cast<unsigned long>(Result)) + ")" });

		return std::filesystem::path(Path.get());
	}

	std::expected<void, PathError> ReplaceFileAtomically(
		const std::filesystem::path& TemporaryPath,
		const std::filesystem::path& DestinationPath)
	{
		if (!MoveFileExW(TemporaryPath.c_str(), DestinationPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
			return std::unexpected(PathError{ "Windows could not replace the settings file (error " +
				std::to_string(GetLastError()) + ")" });
		return {};
	}

	void DebugBreak() noexcept
	{
		::DebugBreak();
	}
} // namespace PulseForge::Platform
