#pragma once

#include "Core/Core.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace PulseForge
{
	enum class DiagnosticEventKind : uint8_t
	{
		Duration,
		Counter,
		Resource
	};

	struct DiagnosticEvent
	{
		uint64_t Sequence = 0;
		uint64_t TimestampNanoseconds = 0;
		uint64_t ThreadToken = 0;
		DiagnosticEventKind Kind = DiagnosticEventKind::Counter;
		std::string Name;
		std::string Owner;
		std::string Unit;
		uint64_t Value = 0;
		uint64_t DurationNanoseconds = 0;
	};

	struct DiagnosticSnapshot
	{
		std::vector<DiagnosticEvent> Events;
		// These timestamps share the steady_clock epoch and are process-local, not wall-clock times.
		uint64_t CaptureStartTimestampNanoseconds = 0;
		uint64_t SnapshotTimestampNanoseconds = 0;
		size_t EventCapacity = 0;
		uint64_t EvictedCount = 0;
		uint64_t DroppedCount = 0;
		bool CaptureEnabled = false;
	};

	class PULSEFORGE_API DiagnosticRecorder final
	{
	public:
		static constexpr size_t DefaultCapacity = 8192;
		static constexpr size_t MaximumCapacity = 8192;
		static constexpr size_t MaximumNameBytes = 96;
		static constexpr size_t MaximumOwnerBytes = 64;
		static constexpr size_t MaximumUnitBytes = 32;

		explicit DiagnosticRecorder(size_t Capacity = DefaultCapacity);

		void SetCaptureEnabled(bool Enabled) noexcept;
		[[nodiscard]] bool IsCaptureEnabled() const noexcept;

		[[nodiscard]] bool RecordEvent(
			DiagnosticEventKind Kind,
			std::string_view Name,
			std::string_view Owner,
			std::string_view Unit = {},
			uint64_t Value = 0,
			uint64_t DurationNanoseconds = 0) noexcept;

		[[nodiscard]] DiagnosticSnapshot CaptureSnapshot() const;
		void Clear() noexcept;

	private:
		struct StoredEvent
		{
			uint64_t Sequence = 0;
			uint64_t TimestampNanoseconds = 0;
			uint64_t ThreadToken = 0;
			DiagnosticEventKind Kind = DiagnosticEventKind::Counter;
			std::array<char, MaximumNameBytes> Name{};
			std::array<char, MaximumOwnerBytes> Owner{};
			std::array<char, MaximumUnitBytes> Unit{};
			uint8_t NameLength = 0;
			uint8_t OwnerLength = 0;
			uint8_t UnitLength = 0;
			uint64_t Value = 0;
			uint64_t DurationNanoseconds = 0;
		};

		const size_t m_Capacity;
		std::unique_ptr<StoredEvent[]> m_Events;
		mutable std::mutex m_Mutex;
		std::atomic<bool> m_CaptureEnabled = false;
		std::atomic<uint64_t> m_CaptureStartTimestampNanoseconds = 0;
		size_t m_FirstEvent = 0;
		size_t m_EventCount = 0;
		uint64_t m_NextSequence = 0;
		uint64_t m_EvictedCount = 0;
		std::atomic<uint64_t> m_DroppedCount = 0;
	};

	class ScopedDiagnosticTimer final
	{
	public:
		ScopedDiagnosticTimer(DiagnosticRecorder& Recorder, std::string_view Name, std::string_view Owner) noexcept;
		ScopedDiagnosticTimer(DiagnosticRecorder* Recorder, std::string_view Name, std::string_view Owner) noexcept;
		~ScopedDiagnosticTimer() noexcept;

		ScopedDiagnosticTimer(const ScopedDiagnosticTimer&) = delete;
		ScopedDiagnosticTimer& operator=(const ScopedDiagnosticTimer&) = delete;
		ScopedDiagnosticTimer(ScopedDiagnosticTimer&&) = delete;
		ScopedDiagnosticTimer& operator=(ScopedDiagnosticTimer&&) = delete;

	private:
		DiagnosticRecorder* m_Recorder = nullptr;
		std::array<char, DiagnosticRecorder::MaximumNameBytes> m_Name{};
		std::array<char, DiagnosticRecorder::MaximumOwnerBytes> m_Owner{};
		uint8_t m_NameLength = 0;
		uint8_t m_OwnerLength = 0;
		uint64_t m_StartTimestampNanoseconds = 0;
		bool m_Started = false;
	};
}
