#include "Core/Diagnostics.h"

#include <algorithm>
#include <chrono>
#include <functional>
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

		template<size_t Capacity>
		[[nodiscard]] uint8_t CopyBounded(std::array<char, Capacity>& Destination, std::string_view Source) noexcept
		{
			const size_t Length = std::min(Capacity, Source.size());
			if (Length > 0)
				std::copy_n(Source.data(), Length, Destination.data());
			return static_cast<uint8_t>(Length);
		}
	}

	DiagnosticRecorder::DiagnosticRecorder(size_t Capacity)
		: m_Capacity(Capacity)
		, m_Events(Capacity > 0 && Capacity <= MaximumCapacity ? std::make_unique<StoredEvent[]>(Capacity) : nullptr)
	{
		if (Capacity == 0 || Capacity > MaximumCapacity)
			throw std::invalid_argument("DiagnosticRecorder capacity must be between 1 and MaximumCapacity");
	}

	void DiagnosticRecorder::SetCaptureEnabled(bool Enabled) noexcept
	{
		if (Enabled)
		{
			m_CaptureStartTimestampNanoseconds.store(GetMonotonicTimestampNanoseconds(), std::memory_order_relaxed);
			m_CaptureEnabled.store(true, std::memory_order_release);
		}
		else
		{
			m_CaptureEnabled.store(false, std::memory_order_release);
		}
	}

	bool DiagnosticRecorder::IsCaptureEnabled() const noexcept
	{
		return m_CaptureEnabled.load(std::memory_order_acquire);
	}

	bool DiagnosticRecorder::RecordEvent(
		DiagnosticEventKind Kind,
		std::string_view Name,
		std::string_view Owner,
		std::string_view Unit,
		uint64_t Value,
		uint64_t DurationNanoseconds) noexcept
	{
		if (!IsCaptureEnabled())
			return false;

		try
		{
			const uint64_t TimestampNanoseconds = GetMonotonicTimestampNanoseconds();
			const uint64_t ThreadToken = static_cast<uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
			std::lock_guard<std::mutex> Lock(m_Mutex);
			if (!m_CaptureEnabled.load(std::memory_order_relaxed))
				return false;
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
			Event.Sequence = ++m_NextSequence;
			Event.TimestampNanoseconds = TimestampNanoseconds;
			Event.ThreadToken = ThreadToken;
			Event.Kind = Kind;
			Event.NameLength = CopyBounded(Event.Name, Name);
			Event.OwnerLength = CopyBounded(Event.Owner, Owner);
			Event.UnitLength = CopyBounded(Event.Unit, Unit);
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

	DiagnosticSnapshot DiagnosticRecorder::CaptureSnapshot() const
	{
		std::lock_guard<std::mutex> Lock(m_Mutex);
		DiagnosticSnapshot Snapshot;
		Snapshot.CaptureEnabled = m_CaptureEnabled.load(std::memory_order_relaxed);
		Snapshot.CaptureStartTimestampNanoseconds = m_CaptureStartTimestampNanoseconds.load(std::memory_order_relaxed);
		Snapshot.SnapshotTimestampNanoseconds = GetMonotonicTimestampNanoseconds();
		Snapshot.EventCapacity = m_Capacity;
		Snapshot.EvictedCount = m_EvictedCount;
		Snapshot.DroppedCount = m_DroppedCount.load(std::memory_order_relaxed);
		Snapshot.Events.reserve(m_EventCount);
		for (size_t Index = 0; Index < m_EventCount; ++Index)
		{
			const StoredEvent& Stored = m_Events[(m_FirstEvent + Index) % m_Capacity];
			DiagnosticEvent Event;
			Event.Sequence = Stored.Sequence;
			Event.TimestampNanoseconds = Stored.TimestampNanoseconds;
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
			m_EvictedCount = 0;
			m_DroppedCount.store(0, std::memory_order_relaxed);
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
		if (!m_Recorder || !m_Recorder->IsCaptureEnabled())
			return;

		m_NameLength = CopyBounded(m_Name, Name);
		m_OwnerLength = CopyBounded(m_Owner, Owner);
		m_StartTimestampNanoseconds = GetMonotonicTimestampNanoseconds();
		m_Started = true;
	}

	ScopedDiagnosticTimer::~ScopedDiagnosticTimer() noexcept
	{
		if (!m_Started)
			return;

		try
		{
			const uint64_t EndTimestampNanoseconds = GetMonotonicTimestampNanoseconds();
			const uint64_t DurationNanoseconds = EndTimestampNanoseconds >= m_StartTimestampNanoseconds
				? EndTimestampNanoseconds - m_StartTimestampNanoseconds
				: 0;
			(void)m_Recorder->RecordEvent(
				DiagnosticEventKind::Duration,
				std::string_view(m_Name.data(), m_NameLength),
				std::string_view(m_Owner.data(), m_OwnerLength),
				"ns",
				0,
				DurationNanoseconds);
		}
		catch (...)
		{
		}
	}
}
