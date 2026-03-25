#include "pfpch.h"
#include "Core/Core.h"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace PulseForge
{
	struct Log::Impl
	{
		std::shared_ptr<spdlog::logger> CoreLogger;
		std::shared_ptr<spdlog::logger> ClientLogger;

		Impl()
		{
			spdlog::set_pattern("%^[%T] %n: %v%$");

			CoreLogger = spdlog::stdout_color_mt("PULSEFORGE");
			CoreLogger->set_level(spdlog::level::trace);

			ClientLogger = spdlog::stdout_color_mt("APP");
			ClientLogger->set_level(spdlog::level::trace);
		}
	};

	std::unique_ptr<Log::Impl> Log::s_Impl;

	void Log::Init()
	{
		s_Impl = std::make_unique<Impl>();
	}

	// Engine wrappers - These now take pre-formatted strings from the header macros!
	void Log::PrintCoreTrace(const std::string& msg) { s_Impl->CoreLogger->trace(msg); }
	void Log::PrintCoreInfo(const std::string& msg) { s_Impl->CoreLogger->info(msg); }
	void Log::PrintCoreWarn(const std::string& msg) { s_Impl->CoreLogger->warn(msg); }
	void Log::PrintCoreError(const std::string& msg) { s_Impl->CoreLogger->error(msg); }
	void Log::PrintCoreCritical(const std::string& msg) { s_Impl->CoreLogger->critical(msg); }

	// Client wrappers - These now take pre-formatted strings from the header macros!
	void Log::PrintTrace(const std::string& msg) { s_Impl->ClientLogger->trace(msg); }
	void Log::PrintInfo(const std::string& msg) { s_Impl->ClientLogger->info(msg); }
	void Log::PrintWarn(const std::string& msg) { s_Impl->ClientLogger->warn(msg); }
	void Log::PrintError(const std::string& msg) { s_Impl->ClientLogger->error(msg); }
	void Log::PrintCritical(const std::string& msg) { s_Impl->ClientLogger->critical(msg); }
}