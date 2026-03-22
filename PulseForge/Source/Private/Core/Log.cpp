#include <pfpch.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include "Core/Log.h"

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

	// Engine wrappers
	template<typename... Args>
	void Log::CoreTrace(const char* fmt, Args&&... args)
	{
		s_Impl->CoreLogger->trace(fmt, std::forward<Args>(args)...);
	}

	template<typename... Args>
	void Log::CoreInfo(const char* fmt, Args&&... args)
	{
		s_Impl->CoreLogger->info(fmt, std::forward<Args>(args)...);
	}

	template<typename... Args>
	void Log::CoreWarn(const char* fmt, Args&&... args)
	{
		s_Impl->CoreLogger->warn(fmt, std::forward<Args>(args)...);
	}

	template<typename... Args>
	void Log::CoreError(const char* fmt, Args&&... args)
	{
		s_Impl->CoreLogger->error(fmt, std::forward<Args>(args)...);
	}

	template<typename... Args>
	void Log::CoreCritical(const char* fmt, Args&&... args)
	{
		s_Impl->CoreLogger->critical(fmt, std::forward<Args>(args)...);
	}

	template<typename... Args>
	void Log::Trace(const char* fmt, Args&&... args)
	{
		s_Impl->ClientLogger->trace(fmt, std::forward<Args>(args)...);
	}

	template<typename... Args>
	void Log::Info(const char* fmt, Args&&... args)
	{
		s_Impl->ClientLogger->info(fmt, std::forward<Args>(args)...);
	}

	template<typename... Args>
	void Log::Warn(const char* fmt, Args&&... args)
	{
		s_Impl->ClientLogger->warn(fmt, std::forward<Args>(args)...);
	}

	template<typename... Args>
	void Log::Error(const char* fmt, Args&&... args)
	{
		s_Impl->ClientLogger->error(fmt, std::forward<Args>(args)...);
	}

	template<typename... Args>
	void Log::Critical(const char* fmt, Args&&... args)
	{
		s_Impl->ClientLogger->critical(fmt, std::forward<Args>(args)...);
	}

	// Explicit template instantiations if needed
	template void Log::CoreTrace<>(const char* fmt);
	template void Log::CoreInfo<>(const char* fmt);
	template void Log::CoreWarn<>(const char* fmt);
	template void Log::CoreError<>(const char* fmt);
	template void Log::CoreCritical<>(const char* fmt);

	template void Log::Trace<>(const char* fmt);
	template void Log::Info<>(const char* fmt);
	template void Log::Warn<>(const char* fmt);
	template void Log::Error<>(const char* fmt);
	template void Log::Critical<>(const char* fmt);
}
