#include "Core/Log.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/ringbuffer_sink.h>
#include <spdlog/sinks/msvc_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <vector>

namespace PulseForge
{
	namespace
	{
		std::shared_ptr<spdlog::logger> CreateLogger(
			const char* Name, const std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt>& HistorySink)
		{
			std::vector<spdlog::sink_ptr> Sinks;
			const HANDLE StandardOutput = GetStdHandle(STD_OUTPUT_HANDLE);
			if (GetConsoleWindow() || (StandardOutput && StandardOutput != INVALID_HANDLE_VALUE))
				Sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
			else Sinks.push_back(std::make_shared<spdlog::sinks::msvc_sink_mt>(false));
			Sinks.push_back(HistorySink);
			auto Logger = std::make_shared<spdlog::logger>(Name, Sinks.begin(), Sinks.end());
			spdlog::register_logger(Logger);
			return Logger;
		}

		[[nodiscard]] LogLevel ConvertLogLevel(spdlog::level::level_enum Level) noexcept
		{
			switch (Level)
			{
			case spdlog::level::trace:
				return LogLevel::Trace;
			case spdlog::level::debug:
				return LogLevel::Debug;
			case spdlog::level::info:
				return LogLevel::Info;
			case spdlog::level::warn:
				return LogLevel::Warning;
			case spdlog::level::err:
				return LogLevel::Error;
			case spdlog::level::critical:
				return LogLevel::Critical;
			default:
				return LogLevel::Unknown;
			}
		}
	} // namespace

	struct Log::Impl
	{
		std::shared_ptr<spdlog::logger> CoreLogger;
		std::shared_ptr<spdlog::logger> ClientLogger;
		std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> HistorySink;

		Impl()
		{
			spdlog::set_pattern("%^[%T] %n: %v%$");

			HistorySink = std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(512);
			CoreLogger = CreateLogger("PULSEFORGE", HistorySink);
			CoreLogger->set_level(spdlog::level::trace);

			ClientLogger = CreateLogger("APP", HistorySink);
			ClientLogger->set_level(spdlog::level::trace);
		}
	};

	std::unique_ptr<Log::Impl> Log::s_Impl;

	void Log::Init()
	{
		s_Impl = std::make_unique<Impl>();
	}

	std::vector<LogEntry> Log::GetRecentEntries()
	{
		std::vector<LogEntry> Entries;
		if (!s_Impl) return Entries;

		const std::vector<spdlog::details::log_msg_buffer> Messages = s_Impl->HistorySink->last_raw();
		Entries.reserve(Messages.size());
		for (const spdlog::details::log_msg_buffer& Message : Messages)
		{
			Entries.push_back({ std::string(Message.logger_name.data(), Message.logger_name.size()), ConvertLogLevel(Message.level),
				std::string(Message.payload.data(), Message.payload.size()) });
		}
		return Entries;
	}

	// Engine wrappers - These now take pre-formatted strings from the header macros!
	void Log::PrintCoreTrace(const std::string& msg)
	{
		s_Impl->CoreLogger->trace(msg);
	}
	void Log::PrintCoreInfo(const std::string& msg)
	{
		s_Impl->CoreLogger->info(msg);
	}
	void Log::PrintCoreWarn(const std::string& msg)
	{
		s_Impl->CoreLogger->warn(msg);
	}
	void Log::PrintCoreError(const std::string& msg)
	{
		s_Impl->CoreLogger->error(msg);
	}
	void Log::PrintCoreCritical(const std::string& msg)
	{
		s_Impl->CoreLogger->critical(msg);
	}

	// Client wrappers - These now take pre-formatted strings from the header macros!
	void Log::PrintTrace(const std::string& msg)
	{
		s_Impl->ClientLogger->trace(msg);
	}
	void Log::PrintInfo(const std::string& msg)
	{
		s_Impl->ClientLogger->info(msg);
	}
	void Log::PrintWarn(const std::string& msg)
	{
		s_Impl->ClientLogger->warn(msg);
	}
	void Log::PrintError(const std::string& msg)
	{
		s_Impl->ClientLogger->error(msg);
	}
	void Log::PrintCritical(const std::string& msg)
	{
		s_Impl->ClientLogger->critical(msg);
	}
} // namespace PulseForge
