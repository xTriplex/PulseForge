#include "Core/Diagnostics.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <limits>
#include <stdexcept>
#include <thread>
#include <utility>

namespace PulseForge
{
	namespace
	{
		[[nodiscard]] uint64_t GetMonotonicTimestampNanoseconds() noexcept
		{
			return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count());
		}

		struct Utf8CopyResult
		{
			uint8_t Length = 0;
			uint64_t MalformedCount = 0;
			bool Truncated = false;
		};

		template<size_t Capacity>
		[[nodiscard]] Utf8CopyResult CopyBoundedUtf8(
			std::array<char, Capacity>& Destination,
			std::string_view Source) noexcept
		{
			constexpr std::string_view Replacement = "\xEF\xBF\xBD";
			Utf8CopyResult Result;
			size_t Input = 0;
			size_t Output = 0;
			while (Input < Source.size())
			{
				const auto First = static_cast<uint8_t>(Source[Input]);
				size_t Width = 0;
				bool Valid = true;
				if (First <= 0x7F) Width = 1;
				else if (First >= 0xC2 && First <= 0xDF) Width = 2;
				else if (First >= 0xE0 && First <= 0xEF) Width = 3;
				else if (First >= 0xF0 && First <= 0xF4) Width = 4;
				else Valid = false;

				if (Valid && Input + Width > Source.size()) Valid = false;
				if (Valid && Width > 1)
				{
					for (size_t Offset = 1; Offset < Width; ++Offset)
					{
						const auto Continuation = static_cast<uint8_t>(Source[Input + Offset]);
						if ((Continuation & 0xC0) != 0x80) Valid = false;
					}
					const auto Second = static_cast<uint8_t>(Source[Input + 1]);
					if ((First == 0xE0 && Second < 0xA0) || (First == 0xED && Second >= 0xA0) ||
						(First == 0xF0 && Second < 0x90) || (First == 0xF4 && Second > 0x8F))
						Valid = false;
				}
				const std::string_view Encoded = Valid ? Source.substr(Input, Width) : Replacement;
				if (Output + Encoded.size() > Capacity)
				{
					Result.Truncated = true;
					break;
				}
				std::copy(Encoded.begin(), Encoded.end(), Destination.begin() + Output);
				Output += Encoded.size();
				Input += Valid ? Width : 1;
				if (!Valid) ++Result.MalformedCount;
			}
			Result.Length = static_cast<uint8_t>(Output);
			return Result;
		}

		void AddUtf8Counts(uint64_t& MalformedCount, uint64_t& TruncatedCount, const Utf8CopyResult& Result) noexcept
		{
			MalformedCount += Result.MalformedCount;
			if (Result.Truncated) ++TruncatedCount;
		}
	}

	DiagnosticRecorder::DiagnosticRecorder(size_t Capacity)
		: m_Capacity(Capacity)
	{
		if (Capacity == 0 || Capacity > MaximumCapacity)
			throw std::invalid_argument("DiagnosticRecorder capacity must be between 1 and MaximumCapacity");
	}

	bool DiagnosticRecorder::SetCaptureEnabled(bool Enabled) noexcept
	{
		try
		{
			std::lock_guard<std::mutex> Lock(m_Mutex);
			const bool WasEnabled = m_CaptureEnabled.load(std::memory_order_relaxed);
			if (Enabled)
			{
				if (WasEnabled) return true;
				if (m_SessionId == UINT64_MAX) return false;
				if (!m_Events) m_Events = std::make_unique<StoredEvent[]>(m_Capacity);
				++m_SessionId;
				m_CaptureStartTimestampNanoseconds = GetMonotonicTimestampNanoseconds();
				m_CaptureEndTimestampNanoseconds = 0;
				m_FirstEvent = 0;
				m_EventCount = 0;
				m_EvictedCount = 0;
				m_DroppedCount.store(0, std::memory_order_relaxed);
				m_MalformedUtf8Count = 0;
				m_TruncatedStringCount = 0;
				m_CaptureEnabled.store(true, std::memory_order_release);
				return true;
			}
			if (WasEnabled)
			{
				m_CaptureEndTimestampNanoseconds = GetMonotonicTimestampNanoseconds();
				m_CaptureEnabled.store(false, std::memory_order_release);
			}
			return true;
		}
		catch (...)
		{
			m_DroppedCount.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
	}

	bool DiagnosticRecorder::IsCaptureEnabled() const noexcept
	{
		return m_CaptureEnabled.load(std::memory_order_acquire);
	}

	bool DiagnosticRecorder::BeginDurationSession(uint64_t& SessionId, uint64_t& StartTimestampNanoseconds) const noexcept
	{
		try
		{
			std::lock_guard<std::mutex> Lock(m_Mutex);
			if (!m_CaptureEnabled.load(std::memory_order_relaxed)) return false;
			SessionId = m_SessionId;
			StartTimestampNanoseconds = GetMonotonicTimestampNanoseconds();
			return true;
		}
		catch (...)
		{
			return false;
		}
	}

	bool DiagnosticRecorder::RecordEvent(
		DiagnosticEventKind Kind,
		std::string_view Name,
		std::string_view Owner,
		std::string_view Unit,
		uint64_t Value,
		uint64_t DurationNanoseconds) noexcept
	{
		try
		{
			std::lock_guard<std::mutex> Lock(m_Mutex);
			if (!m_CaptureEnabled.load(std::memory_order_relaxed)) return false;
			if (m_NextSequence == UINT64_MAX)
			{
				m_DroppedCount.fetch_add(1, std::memory_order_relaxed);
				return false;
			}

			size_t Slot = 0;
			if (m_EventCount < m_Capacity)
			{
				Slot = (m_FirstEvent + m_EventCount) % m_Capacity;
				++m_EventCount;
			}
			else
			{
				Slot = m_FirstEvent;
				m_FirstEvent = (m_FirstEvent + 1) % m_Capacity;
				++m_EvictedCount;
			}

			StoredEvent& Event = m_Events[Slot];
			Event = {};
			Event.SessionId = m_SessionId;
			Event.Sequence = ++m_NextSequence;
			Event.TimestampNanoseconds = GetMonotonicTimestampNanoseconds();
			Event.ThreadToken = static_cast<uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
			Event.Kind = Kind;
			const auto NameCopy = CopyBoundedUtf8(Event.Name, Name);
			const auto OwnerCopy = CopyBoundedUtf8(Event.Owner, Owner);
			const auto UnitCopy = CopyBoundedUtf8(Event.Unit, Unit);
			Event.NameLength = NameCopy.Length;
			Event.OwnerLength = OwnerCopy.Length;
			Event.UnitLength = UnitCopy.Length;
			AddUtf8Counts(m_MalformedUtf8Count, m_TruncatedStringCount, NameCopy);
			AddUtf8Counts(m_MalformedUtf8Count, m_TruncatedStringCount, OwnerCopy);
			AddUtf8Counts(m_MalformedUtf8Count, m_TruncatedStringCount, UnitCopy);
			Event.Value = Value;
			Event.DurationNanoseconds = DurationNanoseconds;
			return true;
		}
		catch (...)
		{
			m_DroppedCount.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
	}

	bool DiagnosticRecorder::RecordDurationForSession(
		uint64_t SessionId,
		uint64_t StartTimestampNanoseconds,
		std::string_view Name,
		std::string_view Owner,
		uint64_t MalformedUtf8Count,
		uint64_t TruncatedStringCount) noexcept
	{
		const uint64_t EndTimestampNanoseconds = GetMonotonicTimestampNanoseconds();
		return RecordDurationForSession(
			SessionId,
			StartTimestampNanoseconds,
			EndTimestampNanoseconds,
			Name,
			Owner,
			MalformedUtf8Count,
			TruncatedStringCount);
	}

	bool DiagnosticRecorder::RecordDurationForSession(
		uint64_t SessionId,
		uint64_t StartTimestampNanoseconds,
		uint64_t EndTimestampNanoseconds,
		std::string_view Name,
		std::string_view Owner,
		uint64_t MalformedUtf8Count,
		uint64_t TruncatedStringCount) noexcept
	{
		try
		{
			std::lock_guard<std::mutex> Lock(m_Mutex);
			if (!m_CaptureEnabled.load(std::memory_order_relaxed) || SessionId != m_SessionId)
			{
				++m_RetiredDurationScopesDiscarded;
				return false;
			}
			if (m_NextSequence == UINT64_MAX)
			{
				m_DroppedCount.fetch_add(1, std::memory_order_relaxed);
				return false;
			}

			size_t Slot = 0;
			if (m_EventCount < m_Capacity)
			{
				Slot = (m_FirstEvent + m_EventCount) % m_Capacity;
				++m_EventCount;
			}
			else
			{
				Slot = m_FirstEvent;
				m_FirstEvent = (m_FirstEvent + 1) % m_Capacity;
				++m_EvictedCount;
			}

			StoredEvent& Event = m_Events[Slot];
			Event = {};
			Event.SessionId = SessionId;
			Event.Sequence = ++m_NextSequence;
			Event.TimestampNanoseconds = EndTimestampNanoseconds;
			Event.StartTimestampNanoseconds = StartTimestampNanoseconds;
			Event.ThreadToken = static_cast<uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
			Event.Kind = DiagnosticEventKind::Duration;
			const auto NameCopy = CopyBoundedUtf8(Event.Name, Name);
			const auto OwnerCopy = CopyBoundedUtf8(Event.Owner, Owner);
			Event.NameLength = NameCopy.Length;
			Event.OwnerLength = OwnerCopy.Length;
			AddUtf8Counts(m_MalformedUtf8Count, m_TruncatedStringCount, NameCopy);
			AddUtf8Counts(m_MalformedUtf8Count, m_TruncatedStringCount, OwnerCopy);
			m_MalformedUtf8Count += MalformedUtf8Count;
			m_TruncatedStringCount += TruncatedStringCount;
			Event.UnitLength = 2;
			Event.Unit[0] = 'n';
			Event.Unit[1] = 's';
			Event.DurationNanoseconds = EndTimestampNanoseconds >= StartTimestampNanoseconds
				? EndTimestampNanoseconds - StartTimestampNanoseconds
				: 0;
			return true;
		}
		catch (...)
		{
			m_DroppedCount.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
	}

	DiagnosticSnapshot DiagnosticRecorder::CaptureSnapshot() const
	{
		std::lock_guard<std::mutex> Lock(m_Mutex);
		DiagnosticSnapshot Snapshot;
		Snapshot.CaptureEnabled = m_CaptureEnabled.load(std::memory_order_relaxed);
		Snapshot.CaptureStartTimestampNanoseconds = m_CaptureStartTimestampNanoseconds;
		Snapshot.CaptureEndTimestampNanoseconds = m_CaptureEndTimestampNanoseconds;
		Snapshot.SnapshotTimestampNanoseconds = GetMonotonicTimestampNanoseconds();
		Snapshot.SessionId = m_SessionId;
		Snapshot.EventCapacity = m_Capacity;
		Snapshot.EvictedCount = m_EvictedCount;
		Snapshot.DroppedCount = m_DroppedCount.load(std::memory_order_relaxed);
		Snapshot.MalformedUtf8Count = m_MalformedUtf8Count;
		Snapshot.TruncatedStringCount = m_TruncatedStringCount;
		Snapshot.RetiredDurationScopesDiscarded = m_RetiredDurationScopesDiscarded;
		Snapshot.Events.reserve(m_EventCount);
		for (size_t Index = 0; Index < m_EventCount; ++Index)
		{
			const StoredEvent& Stored = m_Events[(m_FirstEvent + Index) % m_Capacity];
			DiagnosticEvent Event;
			Event.SessionId = Stored.SessionId;
			Event.Sequence = Stored.Sequence;
			Event.TimestampNanoseconds = Stored.TimestampNanoseconds;
			Event.StartTimestampNanoseconds = Stored.StartTimestampNanoseconds;
			Event.ThreadToken = Stored.ThreadToken;
			Event.Kind = Stored.Kind;
			Event.Name.assign(Stored.Name.data(), Stored.NameLength);
			Event.Owner.assign(Stored.Owner.data(), Stored.OwnerLength);
			Event.Unit.assign(Stored.Unit.data(), Stored.UnitLength);
			Event.Value = Stored.Value;
			Event.DurationNanoseconds = Stored.DurationNanoseconds;
			Snapshot.Events.push_back(std::move(Event));
		}
		return Snapshot;
	}

	void DiagnosticRecorder::Clear() noexcept
	{
		try
		{
			std::lock_guard<std::mutex> Lock(m_Mutex);
			m_FirstEvent = 0;
			m_EventCount = 0;
		}
		catch (...)
		{
			m_DroppedCount.fetch_add(1, std::memory_order_relaxed);
		}
	}

	ScopedDiagnosticTimer::ScopedDiagnosticTimer(DiagnosticRecorder& Recorder, std::string_view Name, std::string_view Owner) noexcept
		: ScopedDiagnosticTimer(&Recorder, Name, Owner)
	{
	}

	ScopedDiagnosticTimer::ScopedDiagnosticTimer(DiagnosticRecorder* Recorder, std::string_view Name, std::string_view Owner) noexcept
		: m_Recorder(Recorder)
	{
		if (!m_Recorder || !m_Recorder->IsCaptureEnabled() ||
			!m_Recorder->BeginDurationSession(m_SessionId, m_StartTimestampNanoseconds))
			return;
		const auto NameCopy = CopyBoundedUtf8(m_Name, Name);
		const auto OwnerCopy = CopyBoundedUtf8(m_Owner, Owner);
		m_NameLength = NameCopy.Length;
		m_OwnerLength = OwnerCopy.Length;
		m_MalformedUtf8Count = NameCopy.MalformedCount + OwnerCopy.MalformedCount;
		m_TruncatedStringCount = static_cast<uint64_t>(NameCopy.Truncated) + static_cast<uint64_t>(OwnerCopy.Truncated);
		m_Started = true;
	}

	ScopedDiagnosticTimer::~ScopedDiagnosticTimer() noexcept
	{
		if (!m_Started) return;
		(void)m_Recorder->RecordDurationForSession(
			m_SessionId,
			m_StartTimestampNanoseconds,
			std::string_view(m_Name.data(), m_NameLength),
			std::string_view(m_Owner.data(), m_OwnerLength),
			m_MalformedUtf8Count,
			m_TruncatedStringCount);
	}
}
