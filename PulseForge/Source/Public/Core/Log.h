#pragma once
#include "Core/Core.h"
#include <memory>
#include <format>
#include <string>

namespace PulseForge
{
	class PULSEFORGE_API Log
	{
	public:
		static void Init();

		// Actual functions implemented in Log.cpp that hide spdlog
		static void PrintCoreTrace(const std::string& msg);
		static void PrintCoreInfo(const std::string& msg);
		static void PrintCoreWarn(const std::string& msg);
		static void PrintCoreError(const std::string& msg);
		static void PrintCoreCritical(const std::string& msg);

		static void PrintTrace(const std::string& msg);
		static void PrintInfo(const std::string& msg);
		static void PrintWarn(const std::string& msg);
		static void PrintError(const std::string& msg);
		static void PrintCritical(const std::string& msg);

		// Magic Inline Templates: these format the string here, then pass it down
		template<typename... Args>
		static void CoreTrace(std::format_string<Args...> fmt, Args&&... args) { PrintCoreTrace(std::format(fmt, std::forward<Args>(args)...)); }

		template<typename... Args>
		static void CoreInfo(std::format_string<Args...> fmt, Args&&... args) { PrintCoreInfo(std::format(fmt, std::forward<Args>(args)...)); }

		template<typename... Args>
		static void CoreWarn(std::format_string<Args...> fmt, Args&&... args) { PrintCoreWarn(std::format(fmt, std::forward<Args>(args)...)); }

		template<typename... Args>
		static void CoreError(std::format_string<Args...> fmt, Args&&... args) { PrintCoreError(std::format(fmt, std::forward<Args>(args)...)); }

		template<typename... Args>
		static void CoreCritical(std::format_string<Args...> fmt, Args&&... args) { PrintCoreCritical(std::format(fmt, std::forward<Args>(args)...)); }


		template<typename... Args>
		static void Trace(std::format_string<Args...> fmt, Args&&... args) { PrintTrace(std::format(fmt, std::forward<Args>(args)...)); }

		template<typename... Args>
		static void Info(std::format_string<Args...> fmt, Args&&... args) { PrintInfo(std::format(fmt, std::forward<Args>(args)...)); }

		template<typename... Args>
		static void Warn(std::format_string<Args...> fmt, Args&&... args) { PrintWarn(std::format(fmt, std::forward<Args>(args)...)); }

		template<typename... Args>
		static void Error(std::format_string<Args...> fmt, Args&&... args) { PrintError(std::format(fmt, std::forward<Args>(args)...)); }

		template<typename... Args>
		static void Critical(std::format_string<Args...> fmt, Args&&... args) { PrintCritical(std::format(fmt, std::forward<Args>(args)...)); }

	private:
		struct Impl;
		static std::unique_ptr<Impl> s_Impl;
	};
}

// ---------------------------------------------
// Engine logging macros
// ---------------------------------------------
#define PF_CORE_TRACE(...)    ::PulseForge::Log::CoreTrace(__VA_ARGS__)
#define PF_CORE_INFO(...)     ::PulseForge::Log::CoreInfo(__VA_ARGS__)
#define PF_CORE_WARN(...)     ::PulseForge::Log::CoreWarn(__VA_ARGS__)
#define PF_CORE_ERROR(...)    ::PulseForge::Log::CoreError(__VA_ARGS__)
#define PF_CORE_CRITICAL(...) ::PulseForge::Log::CoreCritical(__VA_ARGS__)

// ---------------------------------------------
// Client logging macros
// ---------------------------------------------
#define PF_TRACE(...)         ::PulseForge::Log::Trace(__VA_ARGS__)
#define PF_INFO(...)          ::PulseForge::Log::Info(__VA_ARGS__)
#define PF_WARN(...)          ::PulseForge::Log::Warn(__VA_ARGS__)
#define PF_ERROR(...)         ::PulseForge::Log::Error(__VA_ARGS__)
#define PF_CRITICAL(...)      ::PulseForge::Log::Critical(__VA_ARGS__)