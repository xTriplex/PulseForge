#include "Core/Log.h"
#include "Core/Platform/LogSink.h"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/base_sink.h>

#include <deque>
#include <mutex>
#include <vector>

namespace PulseForge
{
	namespace
	{
		[[nodiscard]] LogLevel ConvertLogLevel(spdlog::level::level_enum Level) noexcept;

		class LogHistorySink final : public spdlog::sinks::base_sink<std::mutex>
		{
		public:
			[[nodiscard]] std::vector<LogEntry> GetRecentEntriesAfter(uint64_t Sequence)
			{
				std::lock_guard<std::mutex> Lock(mutex_);
				std::vector<LogEntry> Entries;
				for (const LogEntry& Entry : m_Entries)
					if (Entry.Sequence > Sequence) Entries.push_back(Entry);
				return Entries;
			}

		protected:
			void sink_it_(const spdlog::details::log_msg& Message) override
			{
				m_Entries.push_back({ std::string(Message.logger_name.data(), Message.logger_name.size()), ConvertLogLevel(Message.level),
					std::string(Message.payload.data(), Message.payload.size()), ++m_NextSequence });
				if (m_Entries.size() > Capacity) m_Entries.pop_front();
			}

			void flush_() override {}

		private:
			static constexpr size_t Capacity = 512;
			std::deque<LogEntry> m_Entries;
			uint64_t m_NextSequence = 0;
		};

		std::shared_ptr<spdlog::logger> CreateLogger(const char* Name, const std::shared_ptr<LogHistorySink>& HistorySink)
		{
			std::vector<spdlog::sink_ptr> Sinks{ Platform::Detail::CreateLogSink(), HistorySink };
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
		std::shared_ptr<LogHistorySink> HistorySink;

		Impl()
		{
			spdlog::set_pattern("%^[%T] %n: %v%$");

			HistorySink = std::make_shared<LogHistorySink>();
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

	bool Log::IsInitialized() noexcept
	{
		return s_Impl != nullptr;
	}

	std::vector<LogEntry> Log::GetRecentEntries()
	{
		return GetRecentEntriesAfter(0);
	}

	std::vector<LogEntry> Log::GetRecentEntriesAfter(uint64_t Sequence)
	{
		if (!s_Impl) return {};
		return s_Impl->HistorySink->GetRecentEntriesAfter(Sequence);
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
