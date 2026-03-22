#pragma once
#include "Core/Core.h"
#include <memory>

namespace PulseForge
{
	class Log
	{
	public:
		static void Init();

		template<typename... Args>
		static void CoreTrace(const char* fmt, Args&&... args);
		template<typename... Args>
		static void CoreInfo(const char* fmt, Args&&... args);
		template<typename... Args>
		static void CoreWarn(const char* fmt, Args&&... args);
		template<typename... Args>
		static void CoreError(const char* fmt, Args&&... args);
		template<typename... Args>
		static void CoreCritical(const char* fmt, Args&&... args);

		template<typename... Args>
		static void Trace(const char* fmt, Args&&... args);
		template<typename... Args>
		static void Info(const char* fmt, Args&&... args);
		template<typename... Args>
		static void Warn(const char* fmt, Args&&... args);
		template<typename... Args>
		static void Error(const char* fmt, Args&&... args);
		template<typename... Args>
		static void Critical(const char* fmt, Args&&... args);

	private:
		struct Impl;
		static std::unique_ptr<Impl> s_Impl;
	};
}

// Logging macros
#define PF_CORE_TRACE(...)    ::PulseForge::Log::CoreTrace(__VA_ARGS__)
#define PF_CORE_INFO(...)     ::PulseForge::Log::CoreInfo(__VA_ARGS__)
#define PF_CORE_WARN(...)     ::PulseForge::Log::CoreWarn(__VA_ARGS__)
#define PF_CORE_ERROR(...)    ::PulseForge::Log::CoreError(__VA_ARGS__)
#define PF_CORE_CRITICAL(...) ::PulseForge::Log::CoreCritical(__VA_ARGS__)

#define PF_TRACE(...)    ::PulseForge::Log::Trace(__VA_ARGS__)
#define PF_INFO(...)     ::PulseForge::Log::Info(__VA_ARGS__)
#define PF_WARN(...)     ::PulseForge::Log::Warn(__VA_ARGS__)
#define PF_ERROR(...)    ::PulseForge::Log::Error(__VA_ARGS__)
#define PF_CRITICAL(...) ::PulseForge::Log::Critical(__VA_ARGS__)
