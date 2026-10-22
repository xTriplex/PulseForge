#include "TestSupport.h"

#include "Core/Diagnostics.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
	using namespace PulseForge;

	void TestDisabledRecording(TestRunner& Tests)
	{
		DiagnosticRecorder Recorder(4);
		PF_CHECK(Tests, !Recorder.IsCaptureEnabled());
		PF_CHECK(Tests, Recorder.SetCaptureEnabled(false));
		PF_CHECK(Tests, !Recorder.RecordEvent(DiagnosticEventKind::Counter, "ignored", "test", "items", 7));

		const DiagnosticSnapshot Snapshot = Recorder.CaptureSnapshot();
		PF_CHECK(Tests, !Snapshot.CaptureEnabled);
		PF_CHECK(Tests, Snapshot.SessionId == 0);
		PF_CHECK(Tests, Snapshot.CaptureStartTimestampNanoseconds == 0);
		PF_CHECK(Tests, Snapshot.CaptureEndTimestampNanoseconds == 0);
		PF_CHECK(Tests, Snapshot.Events.empty());
		PF_CHECK(Tests, Snapshot.EventCapacity == 4);
		PF_CHECK(Tests, Snapshot.EvictedCount == 0);
		PF_CHECK(Tests, Snapshot.DroppedCount == 0);
	}

	void TestCapacityAndClear(TestRunner& Tests)
	{
		DiagnosticRecorder Recorder(3);
		Recorder.SetCaptureEnabled(true);
		const uint64_t FirstSessionId = Recorder.CaptureSnapshot().SessionId;
		for (uint64_t Value = 1; Value <= 5; ++Value)
			PF_CHECK(Tests, Recorder.RecordEvent(DiagnosticEventKind::Counter, "count", "test", "items", Value));

		const DiagnosticSnapshot BeforeClear = Recorder.CaptureSnapshot();
		PF_CHECK(Tests, BeforeClear.Events.size() == 3);
		PF_CHECK(Tests, BeforeClear.Events[0].Value == 3);
		PF_CHECK(Tests, BeforeClear.Events[1].Value == 4);
		PF_CHECK(Tests, BeforeClear.Events[2].Value == 5);
		PF_CHECK(Tests, BeforeClear.Events[0].Sequence < BeforeClear.Events[1].Sequence);
		PF_CHECK(Tests, BeforeClear.Events[1].Sequence < BeforeClear.Events[2].Sequence);
		PF_CHECK(Tests, BeforeClear.EvictedCount == 2);
		PF_CHECK(Tests, BeforeClear.DroppedCount == 0);

		Recorder.Clear();
		PF_CHECK(Tests, Recorder.RecordEvent(DiagnosticEventKind::Resource, "created", "test", "objects", 6));
		const DiagnosticSnapshot AfterClear = Recorder.CaptureSnapshot();
		PF_CHECK(Tests, AfterClear.Events.size() == 1);
		PF_CHECK(Tests, AfterClear.Events[0].Value == 6);
		PF_CHECK(Tests, AfterClear.Events[0].Sequence > BeforeClear.Events.back().Sequence);
		PF_CHECK(Tests, AfterClear.SessionId == FirstSessionId);
		PF_CHECK(Tests, AfterClear.EvictedCount == BeforeClear.EvictedCount);
		PF_CHECK(Tests, AfterClear.DroppedCount == 0);
		PF_CHECK(Tests, BeforeClear.Events.size() == 3);
		PF_CHECK(Tests, BeforeClear.Events[0].Value == 3);
		PF_CHECK(Tests, BeforeClear.Events[2].Value == 5);
	}

	void TestBoundedStrings(TestRunner& Tests)
	{
		DiagnosticRecorder Recorder(1);
		Recorder.SetCaptureEnabled(true);
		const std::string Name(DiagnosticRecorder::MaximumNameBytes + 11, 'n');
		const std::string Owner(DiagnosticRecorder::MaximumOwnerBytes + 9, 'o');
		const std::string Unit(DiagnosticRecorder::MaximumUnitBytes + 5, 'u');
		PF_CHECK(Tests, Recorder.RecordEvent(DiagnosticEventKind::Counter, Name, Owner, Unit, 42));

		const auto Snapshot = Recorder.CaptureSnapshot();
		PF_CHECK(Tests, Snapshot.Events.size() == 1);
		if (Snapshot.Events.empty()) return;
		PF_CHECK(Tests, Snapshot.Events[0].Name.size() == DiagnosticRecorder::MaximumNameBytes);
		PF_CHECK(Tests, Snapshot.Events[0].Owner.size() == DiagnosticRecorder::MaximumOwnerBytes);
		PF_CHECK(Tests, Snapshot.Events[0].Unit.size() == DiagnosticRecorder::MaximumUnitBytes);
		PF_CHECK(Tests, Snapshot.Events[0].Name == std::string(DiagnosticRecorder::MaximumNameBytes, 'n'));
		PF_CHECK(Tests, Snapshot.Events[0].Owner == std::string(DiagnosticRecorder::MaximumOwnerBytes, 'o'));
		PF_CHECK(Tests, Snapshot.Events[0].Unit == std::string(DiagnosticRecorder::MaximumUnitBytes, 'u'));
	}

	void TestCaptureSessionBoundariesAndRetiredTimer(TestRunner& Tests)
	{
		DiagnosticRecorder Recorder(4);
		const uint64_t DiscardedScopesBefore = Recorder.CaptureSnapshot().RetiredDurationScopesDiscarded;
		Recorder.SetCaptureEnabled(true);
		const DiagnosticSnapshot Started = Recorder.CaptureSnapshot();
		PF_CHECK(Tests, Started.SessionId != 0);
		PF_CHECK(Tests, Started.CaptureEnabled);
		PF_CHECK(Tests, Started.CaptureStartTimestampNanoseconds != 0);
		PF_CHECK(Tests, Started.CaptureEndTimestampNanoseconds == 0);
		PF_CHECK(Tests, Recorder.RecordEvent(DiagnosticEventKind::Counter, "before restart", "test", "items", 1));
		const DiagnosticSnapshot FirstSessionEvent = Recorder.CaptureSnapshot();
		PF_CHECK(Tests, FirstSessionEvent.Events.size() == 1);
		if (FirstSessionEvent.Events.empty()) return;
		const uint64_t FirstSessionSequence = FirstSessionEvent.Events.back().Sequence;

		{
			ScopedDiagnosticTimer RetiredTimer(Recorder, "retired timer", "test");
			Recorder.SetCaptureEnabled(false);
			const DiagnosticSnapshot Stopped = Recorder.CaptureSnapshot();
			PF_CHECK(Tests, !Stopped.CaptureEnabled);
			PF_CHECK(Tests, Stopped.CaptureEndTimestampNanoseconds >= Stopped.CaptureStartTimestampNanoseconds);
			Recorder.SetCaptureEnabled(true);
			PF_CHECK(Tests, Recorder.CaptureSnapshot().SessionId > Started.SessionId);
		}

		const DiagnosticSnapshot AfterRetiredTimer = Recorder.CaptureSnapshot();
		PF_CHECK(Tests, AfterRetiredTimer.Events.empty());
		PF_CHECK(Tests, AfterRetiredTimer.CaptureEndTimestampNanoseconds == 0);
		PF_CHECK(Tests, AfterRetiredTimer.SessionId > Started.SessionId);
		PF_CHECK(Tests, AfterRetiredTimer.RetiredDurationScopesDiscarded == DiscardedScopesBefore + 1);

		{
			ScopedDiagnosticTimer ActiveTimer(Recorder, "active timer", "test");
		}
		const DiagnosticSnapshot Completed = Recorder.CaptureSnapshot();
		PF_CHECK(Tests, Completed.Events.size() == 1);
		if (Completed.Events.empty()) return;
		PF_CHECK(Tests, Completed.Events[0].SessionId == Completed.SessionId);
		PF_CHECK(Tests, Completed.Events[0].StartTimestampNanoseconds >= Completed.CaptureStartTimestampNanoseconds);
		PF_CHECK(Tests, Completed.Events[0].TimestampNanoseconds >= Completed.Events[0].StartTimestampNanoseconds);
		PF_CHECK(Tests, Completed.Events[0].DurationNanoseconds ==
			Completed.Events[0].TimestampNanoseconds - Completed.Events[0].StartTimestampNanoseconds);
		PF_CHECK(Tests, Completed.Events[0].Sequence > FirstSessionSequence);

		Recorder.SetCaptureEnabled(false);
		const DiagnosticSnapshot Stopped = Recorder.CaptureSnapshot();
		const DiagnosticSnapshot StillStopped = Recorder.CaptureSnapshot();
		PF_CHECK(Tests, Stopped.CaptureEndTimestampNanoseconds != 0);
		PF_CHECK(Tests, StillStopped.CaptureEndTimestampNanoseconds == Stopped.CaptureEndTimestampNanoseconds);
		PF_CHECK(Tests, StillStopped.SnapshotTimestampNanoseconds >= Stopped.SnapshotTimestampNanoseconds);
		Recorder.SetCaptureEnabled(true);
		const DiagnosticSnapshot NextSession = Recorder.CaptureSnapshot();
		PF_CHECK(Tests, NextSession.SessionId > Completed.SessionId);
		PF_CHECK(Tests, NextSession.CaptureEndTimestampNanoseconds == 0);
		PF_CHECK(Tests, NextSession.Events.empty());
		PF_CHECK(Tests, NextSession.EvictedCount == 0 && NextSession.DroppedCount == 0);
	}

	void TestUtf8RepairAndFieldTruncation(TestRunner& Tests)
	{
		DiagnosticRecorder Recorder(4);
		Recorder.SetCaptureEnabled(true);
		const std::string MalformedName = std::string("bad") + static_cast<char>(0xff) + "name";
		const std::string TruncatedName = std::string(DiagnosticRecorder::MaximumNameBytes - 1, 'n') + "\xc3\xa9";
		const std::string LongOwner(DiagnosticRecorder::MaximumOwnerBytes + 1, 'o');
		const std::string LongUnit(DiagnosticRecorder::MaximumUnitBytes + 1, 'u');
		PF_CHECK(Tests, Recorder.RecordEvent(DiagnosticEventKind::Counter, MalformedName, "owner", "items", 1));
		PF_CHECK(Tests, Recorder.RecordEvent(DiagnosticEventKind::Counter, TruncatedName, LongOwner, LongUnit, 2));

		const DiagnosticSnapshot Snapshot = Recorder.CaptureSnapshot();
		PF_CHECK(Tests, Snapshot.Events.size() == 2);
		PF_CHECK(Tests, Snapshot.MalformedUtf8Count == 1);
		PF_CHECK(Tests, Snapshot.TruncatedStringCount == 3);
		if (Snapshot.Events.size() != 2) return;
		PF_CHECK(Tests, Snapshot.Events[0].Name == std::string("bad\xef\xbf\xbdname"));
		PF_CHECK(Tests, Snapshot.Events[1].Name == std::string(DiagnosticRecorder::MaximumNameBytes - 1, 'n'));
		PF_CHECK(Tests, Snapshot.Events[1].Owner.size() == DiagnosticRecorder::MaximumOwnerBytes);
		PF_CHECK(Tests, Snapshot.Events[1].Unit.size() == DiagnosticRecorder::MaximumUnitBytes);

		Recorder.Clear();
		const DiagnosticSnapshot Cleared = Recorder.CaptureSnapshot();
		PF_CHECK(Tests, Cleared.Events.empty());
		PF_CHECK(Tests, Cleared.MalformedUtf8Count == Snapshot.MalformedUtf8Count);
		PF_CHECK(Tests, Cleared.TruncatedStringCount == Snapshot.TruncatedStringCount);
		Recorder.SetCaptureEnabled(false);
		Recorder.SetCaptureEnabled(true);
		const DiagnosticSnapshot FreshSession = Recorder.CaptureSnapshot();
		PF_CHECK(Tests, FreshSession.SessionId > Snapshot.SessionId);
		PF_CHECK(Tests, FreshSession.MalformedUtf8Count == 0);
		PF_CHECK(Tests, FreshSession.TruncatedStringCount == 0);
	}

	void TestConcurrentRecordingAndSnapshots(TestRunner& Tests)
	{
		constexpr uint64_t ProducerCount = 4;
		constexpr uint64_t EventsPerProducer = 128;
		DiagnosticRecorder Recorder(ProducerCount * EventsPerProducer);
		Recorder.SetCaptureEnabled(true);
		std::atomic<bool> Started = false;
		std::atomic<uint64_t> FinishedProducers = 0;
		std::vector<std::thread> Producers;
		Producers.reserve(ProducerCount);
		for (uint64_t Producer = 0; Producer < ProducerCount; ++Producer)
		{
			Producers.emplace_back([&, Producer]
			{
				Started.store(true, std::memory_order_release);
				for (uint64_t Index = 0; Index < EventsPerProducer; ++Index)
				{
					const uint64_t Value = Producer * EventsPerProducer + Index;
					(void)Recorder.RecordEvent(DiagnosticEventKind::Counter, "parallel", "producer", "id", Value);
				}
				FinishedProducers.fetch_add(1, std::memory_order_release);
			});
		}

		while (!Started.load(std::memory_order_acquire)) std::this_thread::yield();
		bool SnapshotsCoherent = true;
		while (FinishedProducers.load(std::memory_order_acquire) < ProducerCount)
		{
			const DiagnosticSnapshot Snapshot = Recorder.CaptureSnapshot();
			for (size_t Index = 1; Index < Snapshot.Events.size(); ++Index)
				SnapshotsCoherent = SnapshotsCoherent && Snapshot.Events[Index - 1].Sequence < Snapshot.Events[Index].Sequence;
			if (Snapshot.Events.size() > ProducerCount * EventsPerProducer) SnapshotsCoherent = false;
			std::this_thread::yield();
		}
		for (std::thread& Producer : Producers)
			if (Producer.joinable()) Producer.join();

		const DiagnosticSnapshot Snapshot = Recorder.CaptureSnapshot();
		PF_CHECK(Tests, SnapshotsCoherent);
		PF_CHECK(Tests, Snapshot.Events.size() == ProducerCount * EventsPerProducer);
		PF_CHECK(Tests, Snapshot.EvictedCount == 0);
		PF_CHECK(Tests, Snapshot.DroppedCount == 0);
		bool ValuesSeen[ProducerCount * EventsPerProducer]{};
		for (size_t Index = 0; Index < Snapshot.Events.size(); ++Index)
		{
			const DiagnosticEvent& Event = Snapshot.Events[Index];
			PF_CHECK(Tests, Event.SessionId == Snapshot.SessionId);
			PF_CHECK(Tests, Event.Sequence == Index + 1);
			PF_CHECK(Tests, Event.Kind == DiagnosticEventKind::Counter);
			PF_CHECK(Tests, Event.Name == "parallel");
			PF_CHECK(Tests, Event.Owner == "producer");
			PF_CHECK(Tests, Event.Unit == "id");
			if (Event.Value < ProducerCount * EventsPerProducer)
				ValuesSeen[Event.Value] = true;
			else
				SnapshotsCoherent = false;
		}
		for (bool Seen : ValuesSeen) PF_CHECK(Tests, Seen);
		PF_CHECK(Tests, SnapshotsCoherent);
	}

	void TestScopedTimerCapture(TestRunner& Tests)
	{
		DiagnosticRecorder Recorder(4);
		{
			ScopedDiagnosticTimer Timer(Recorder, "disabled timer", "test");
		}
		PF_CHECK(Tests, Recorder.CaptureSnapshot().Events.empty());

		Recorder.SetCaptureEnabled(true);
		{
			ScopedDiagnosticTimer Timer(Recorder, "enabled timer", "test");
		}
		const auto Snapshot = Recorder.CaptureSnapshot();
		PF_CHECK(Tests, Snapshot.Events.size() == 1);
		if (Snapshot.Events.empty()) return;
		PF_CHECK(Tests, Snapshot.Events[0].Kind == DiagnosticEventKind::Duration);
		PF_CHECK(Tests, Snapshot.Events[0].Name == "enabled timer");
		PF_CHECK(Tests, Snapshot.Events[0].Owner == "test");
		PF_CHECK(Tests, Snapshot.Events[0].Unit == "ns");
	}

	void TestInvalidCapacities(TestRunner& Tests)
	{
		bool RejectedZero = false;
		try { DiagnosticRecorder Recorder(0); }
		catch (const std::invalid_argument&) { RejectedZero = true; }
		PF_CHECK(Tests, RejectedZero);

		bool RejectedTooLarge = false;
		try { DiagnosticRecorder Recorder(DiagnosticRecorder::MaximumCapacity + 1); }
		catch (const std::invalid_argument&) { RejectedTooLarge = true; }
		PF_CHECK(Tests, RejectedTooLarge);
	}
}

void RunDiagnosticsRecorderTests(TestRunner& Tests)
{
	using namespace PulseForge;
	TestDisabledRecording(Tests);
	TestCapacityAndClear(Tests);
	TestBoundedStrings(Tests);
	TestCaptureSessionBoundariesAndRetiredTimer(Tests);
	TestUtf8RepairAndFieldTruncation(Tests);
	TestConcurrentRecordingAndSnapshots(Tests);
	TestScopedTimerCapture(Tests);
	TestInvalidCapacities(Tests);
	if (std::getenv("PULSEFORGE_DIAGNOSTICS_BENCHMARK"))
	{
		using Clock = std::chrono::steady_clock;
		constexpr size_t Iterations = 200'000;
		DiagnosticRecorder Recorder;
		auto Begin = Clock::now();
		for (size_t Index = 0; Index < Iterations; ++Index)
		{
			ScopedDiagnosticTimer Timer(Recorder, "benchmark", "test");
		}
		const auto DisabledElapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - Begin).count();
		Recorder.SetCaptureEnabled(true);
		Begin = Clock::now();
		for (size_t Index = 0; Index < Iterations; ++Index)
		{
			ScopedDiagnosticTimer Timer(Recorder, "benchmark", "test");
		}
		const auto EnabledElapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - Begin).count();
		std::cout << "PULSEFORGE_DIAGNOSTICS_BENCHMARK; iterations=" << Iterations
			<< "; disabled_ns_per_scope=" << (DisabledElapsed / static_cast<int64_t>(Iterations))
			<< "; enabled_ns_per_scope=" << (EnabledElapsed / static_cast<int64_t>(Iterations))
			<< "; ring_capacity=" << Recorder.CaptureSnapshot().EventCapacity << '\n';
	}
}
