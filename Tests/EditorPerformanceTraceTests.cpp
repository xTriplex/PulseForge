#include "TestSupport.h"

#include "Editor/EditorPerformanceCaptureService.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <utility>

namespace
{
	PulseForgeEditor::EditorPerformanceSnapshot MakeSnapshot(
		uint64_t SessionId, uint64_t SnapshotTime, bool Active = true)
	{
		PulseForgeEditor::EditorPerformanceSnapshot Snapshot;
		Snapshot.Diagnostics.SessionId = SessionId;
		Snapshot.Diagnostics.CaptureStartTimestampNanoseconds = 1'000;
		Snapshot.Diagnostics.CaptureEndTimestampNanoseconds = Active ? 0 : SnapshotTime;
		Snapshot.Diagnostics.SnapshotTimestampNanoseconds = SnapshotTime;
		Snapshot.Diagnostics.CaptureEnabled = Active;
		Snapshot.Diagnostics.EventCapacity = 8;
		return Snapshot;
	}

	PulseForgeEditor::EditorPerformanceCaptureMetadata MakeMetadata(std::string Workload, std::string Fingerprint = "project-a")
	{
		return { .Platform = "Windows", .Configuration = "Debug", .Renderer = "Vulkan",
			.Workload = std::move(Workload), .BuildIdentifier = "test-build",
			.ContentFingerprint = std::move(Fingerprint) };
	}

	PulseForgeEditor::RendererTimingObservation MakeRendererObservation(
		const PulseForgeEditor::EditorPerformanceSnapshot& Snapshot,
		PulseForgeEditor::RendererTimingLifetimeCounters Counters = {})
	{
		PulseForgeEditor::RendererTimingObservation Observation;
		Observation.CaptureSessionId = Snapshot.Diagnostics.SessionId;
		Observation.LifetimeCounters = Counters;
		Observation.Supported = Snapshot.GpuTimingSupported;
		Observation.Requested = Snapshot.GpuTimingRequested;
		Observation.Enabled = Snapshot.GpuTimingEnabled;
		Observation.FailureReported = Snapshot.GpuTimingFailureReported;
		Observation.CollectionOutstanding = Snapshot.Diagnostics.CaptureEnabled && Snapshot.GpuTimingEnabled;
		Observation.UnavailableReason = Snapshot.GpuTimingUnavailableReason;
		Observation.NewlyCompletedSamples = Snapshot.CompletedGpuTimings;
		return Observation;
	}

	std::expected<void, PulseForgeEditor::EditorPerformanceCaptureError> BeginCapture(
		PulseForgeEditor::EditorPerformanceCaptureService& Service,
		const PulseForgeEditor::EditorPerformanceSnapshot& Snapshot,
		PulseForgeEditor::EditorPerformanceCaptureMetadata Metadata,
		PulseForgeEditor::RendererTimingLifetimeCounters Counters = {})
	{
		const auto Observation = MakeRendererObservation(Snapshot, Counters);
		return Service.Begin(Snapshot.Diagnostics, Observation, Snapshot.Resources, std::move(Metadata));
	}

	std::expected<void, PulseForgeEditor::EditorPerformanceCaptureError> ObserveCapture(
		PulseForgeEditor::EditorPerformanceCaptureService& Service,
		const PulseForgeEditor::EditorPerformanceSnapshot& Snapshot,
		const PulseForgeEditor::EditorPerformanceCaptureMetadata& Metadata,
		PulseForgeEditor::RendererTimingLifetimeCounters Counters = {})
	{
		const auto Observation = MakeRendererObservation(Snapshot, Counters);
		return Service.Observe(Observation, Snapshot.Diagnostics.SnapshotTimestampNanoseconds,
			&Snapshot.Resources, &Metadata);
	}

	std::expected<void, PulseForgeEditor::EditorPerformanceCaptureError> StopCapture(
		PulseForgeEditor::EditorPerformanceCaptureService& Service,
		const PulseForgeEditor::EditorPerformanceSnapshot& Snapshot,
		PulseForgeEditor::EditorPerformanceCaptureMetadata Metadata,
		PulseForgeEditor::RendererTimingLifetimeCounters Counters = {})
	{
		const auto Observation = MakeRendererObservation(Snapshot, Counters);
		return Service.Stop(Snapshot.Diagnostics, Observation, Snapshot.Resources, std::move(Metadata));
	}
}

void RunEditorPerformanceTraceTests(TestRunner& Tests)
{
	using namespace PulseForgeEditor;
	using PulseForge::DiagnosticEvent;
	using PulseForge::DiagnosticEventKind;

	PulseForge::DiagnosticRecorder Recorder(8);
	Recorder.SetCaptureEnabled(true);
	{
		PulseForge::ScopedDiagnosticTimer RetiredTimer(Recorder, "retired timer", "SceneRuntime");
		Recorder.SetCaptureEnabled(false);
		Recorder.SetCaptureEnabled(true);
	}
	PF_CHECK(Tests, Recorder.CaptureSnapshot().RetiredDurationScopesDiscarded == 1);
	PF_CHECK(Tests, Recorder.RecordEvent(DiagnosticEventKind::Counter, "cache hits", "PreviewCache", "hits", 12));
	{
		PulseForge::ScopedDiagnosticTimer Timer(Recorder, "scene update", "SceneRuntime");
	}
	EditorPerformanceSnapshot Initial;
	Initial.Diagnostics = Recorder.CaptureSnapshot();
	Initial.GpuTimingSupported = true;
	Initial.GpuTimingRequested = true;
	Initial.GpuTimingEnabled = true;
	Initial.Resources.push_back({ "Preview bytes", "PreviewCache", "Logical texture footprint", 4096,
		EditorResourceMetricKind::Bytes, EditorResourceMetricQuality::LogicalAllocation, {}, "estimate" });
	Initial.Resources.push_back({ "Native image bytes", "Renderer", "Not provided by backend", 0,
		EditorResourceMetricKind::Bytes, EditorResourceMetricQuality::Unavailable,
		"backend reports no native allocation size", {} });

	EditorPerformanceCaptureService CaptureService;
	auto BeginResult = BeginCapture(CaptureService, Initial, MakeMetadata("cold-open"));
	PF_CHECK(Tests, BeginResult.has_value());
	PF_CHECK(Tests, !BeginCapture(CaptureService, Initial, MakeMetadata("duplicate")));
	const auto ActiveCopy = CaptureService.GetExportSnapshot();
	PF_CHECK(Tests, ActiveCopy.has_value());
	PF_CHECK(Tests, ActiveCopy && ActiveCopy->Snapshot.Diagnostics.CaptureEndTimestampNanoseconds == 0);
	auto ActiveSerialized = ActiveCopy ? EditorPerformanceCaptureService::Serialize(*ActiveCopy) :
		std::expected<std::string, EditorPerformanceExportError>(std::unexpected(EditorPerformanceExportError{}));
	PF_CHECK(Tests, ActiveSerialized.has_value());
	if (ActiveSerialized)
	{
		const nlohmann::json Trace = nlohmann::json::parse(*ActiveSerialized);
		PF_CHECK(Tests, Trace["metadata"]["capture_enabled_at_export"] == true);
		PF_CHECK(Tests, Trace["metadata"]["gpu_timing_requested"] == true);
		PF_CHECK(Tests, Trace["metadata"]["gpu_timing_enabled"] == true);
		PF_CHECK(Tests, Trace["metadata"]["capture_stop_monotonic_ns"] == 0);
		PF_CHECK(Tests, Trace["metadata"]["snapshot_monotonic_ns"] == Initial.Diagnostics.SnapshotTimestampNanoseconds);
		PF_CHECK(Tests, Trace["metadata"]["workload"] == "cold-open");
	}

	EditorPerformanceSnapshot Observe = MakeSnapshot(Initial.Diagnostics.SessionId, Initial.Diagnostics.SnapshotTimestampNanoseconds + 10);
	Observe.Diagnostics.CaptureStartTimestampNanoseconds = Initial.Diagnostics.CaptureStartTimestampNanoseconds;
	Observe.Diagnostics.Events = Initial.Diagnostics.Events;
	Observe.Resources = Initial.Resources;
	Observe.GpuTimingSupported = true;
	Observe.GpuTimingRequested = true;
	Observe.GpuTimingEnabled = true;
	for (uint64_t Index = 0; Index < 80; ++Index)
	{
		Observe.CompletedGpuTimings.push_back({ Observe.Diagnostics.SessionId, "frame", "VulkanRenderer", "query",
			Index, 2'000 + Index, Index == 0 ? 0 : 100 + Index });
	}
	auto ObserveResult = ObserveCapture(CaptureService, Observe, MakeMetadata("warm-open", "project-b"),
		{ .RendererGeneration = 1, .QueueOverwriteCount = 0, .InvalidResultCount = 2 });
	PF_CHECK(Tests, ObserveResult.has_value());
	const auto WrongSnapshot = MakeSnapshot(Initial.Diagnostics.SessionId + 99, 3'000);
	const auto WrongObservation = MakeRendererObservation(WrongSnapshot);
	const auto WrongSession = CaptureService.Observe(WrongObservation, 3'000);
	PF_CHECK(Tests, !WrongSession && WrongSession.error().Code == EditorPerformanceCaptureErrorCode::StaleSession);

	EditorPerformanceSnapshot Final = MakeSnapshot(Initial.Diagnostics.SessionId, Observe.Diagnostics.SnapshotTimestampNanoseconds + 10, false);
	Final.Diagnostics.CaptureStartTimestampNanoseconds = Initial.Diagnostics.CaptureStartTimestampNanoseconds;
	Final.Diagnostics.Events = Observe.Diagnostics.Events;
	Final.Resources = Observe.Resources;
	Final.GpuTimingSupported = true;
	Final.GpuTimingRequested = true;
	Final.GpuTimingEnabled = false;
	PF_CHECK(Tests, StopCapture(CaptureService, Final, MakeMetadata("warm-open", "project-b"),
		{ .RendererGeneration = 1, .QueueOverwriteCount = 0, .InvalidResultCount = 2 }).has_value());
	PF_CHECK(Tests, !CaptureService.IsActive());
	const auto Completed = CaptureService.GetExportSnapshot();
	PF_CHECK(Tests, Completed.has_value());
	if (!Completed) return;
	PF_CHECK(Tests, Completed->Snapshot.GpuTimingResultStatus == EditorGpuTimingResultStatus::Informative);
	PF_CHECK(Tests, Completed->Snapshot.GpuTimingAvailable);
	PF_CHECK(Tests, Completed->Snapshot.Timing.CaptureHistoryEvictionCount == 16);
	PF_CHECK(Tests, Completed->Snapshot.Timing.RendererQueueOverwriteCount == 0);
	PF_CHECK(Tests, Completed->Snapshot.Timing.InvalidResultCount == 2);
	PF_CHECK(Tests, Completed->Snapshot.Timing.StaleSessionDiscardCount == 0);
	PF_CHECK(Tests, Completed->Snapshot.Timing.ZeroDurationCount == 1);
	PF_CHECK(Tests, Completed->Snapshot.CompletedGpuTimings.size() == 64);
	PF_CHECK(Tests, Completed->Snapshot.CompletedGpuTimings.front().FrameSequence == 16);
	PF_CHECK(Tests, Completed->Snapshot.CompletedGpuTimings.back().FrameSequence == 79);
	PF_CHECK(Tests, Completed->ResourceSegments.size() == 2);
	PF_CHECK(Tests, Completed->ResourceSegments.front().Workload == "cold-open");
	PF_CHECK(Tests, Completed->ResourceSegments.back().Workload == "warm-open");
	PF_CHECK(Tests, Completed->ResourceSegments.back().ContentFingerprint == "project-b");
	PF_CHECK(Tests, Completed->Snapshot.Timing.PositiveDurationCount == 79);

	const auto Serialized = EditorPerformanceCaptureService::Serialize(*Completed);
	PF_CHECK(Tests, Serialized.has_value());
	if (!Serialized) return;
	const nlohmann::json Trace = nlohmann::json::parse(*Serialized);
	PF_CHECK(Tests, Trace["metadata"]["capture_id"] == Initial.Diagnostics.SessionId);
	PF_CHECK(Tests, Trace["metadata"]["gpu_timing_result_status"] == "informative");
	PF_CHECK(Tests, Trace["metadata"]["gpu_timing_renderer_overwrite_count"] == 0);
	PF_CHECK(Tests, Trace["metadata"]["gpu_timing_capture_history_eviction_count"] == 16);
	PF_CHECK(Tests, Trace["metadata"]["gpu_timing_invalid_result_count"] == 2);
	PF_CHECK(Tests, Trace["metadata"]["gpu_timing_export_truncation_count"] == 0);
	PF_CHECK(Tests, Trace["metadata"]["resource_segments"].size() == 2);
	PF_CHECK(Tests, Trace["metadata"]["gpu_timings"].size() == 64);
	PF_CHECK(Tests, Trace["metadata"]["gpu_timings"][0]["frame_sequence"] == 16);
	PF_CHECK(Tests, Trace["metadata"]["gpu_timings"][0]["quality"] == "positive-duration");
	PF_CHECK(Tests, Trace["metadata"]["resources"][1]["quality"] == "unavailable");
	PF_CHECK(Tests, Trace["metadata"]["resources"][1]["unavailable_reason"] == "backend reports no native allocation size");
	PF_CHECK(Tests, Trace["traceEvents"].size() == 2);
	if (Trace["traceEvents"].size() == 2)
	{
		const auto CounterEvent = std::find_if(Initial.Diagnostics.Events.begin(), Initial.Diagnostics.Events.end(),
			[](const DiagnosticEvent& Event) { return Event.Kind == DiagnosticEventKind::Counter; });
		const auto DurationEvent = std::find_if(Initial.Diagnostics.Events.begin(), Initial.Diagnostics.Events.end(),
			[](const DiagnosticEvent& Event) { return Event.Kind == DiagnosticEventKind::Duration; });
		const auto ExportedCounter = std::find_if(Trace["traceEvents"].begin(), Trace["traceEvents"].end(),
			[](const nlohmann::json& Event) { return Event["name"] == "cache hits"; });
		const auto ExportedDuration = std::find_if(Trace["traceEvents"].begin(), Trace["traceEvents"].end(),
			[](const nlohmann::json& Event) { return Event["name"] == "scene update"; });
		PF_CHECK(Tests, CounterEvent != Initial.Diagnostics.Events.end() && ExportedCounter != Trace["traceEvents"].end());
		PF_CHECK(Tests, DurationEvent != Initial.Diagnostics.Events.end() && ExportedDuration != Trace["traceEvents"].end());
		if (CounterEvent != Initial.Diagnostics.Events.end() && ExportedCounter != Trace["traceEvents"].end())
		{
			PF_CHECK(Tests, (*ExportedCounter)["ph"] == "C");
			const double Expected = static_cast<double>(CounterEvent->TimestampNanoseconds -
				Initial.Diagnostics.CaptureStartTimestampNanoseconds) / 1000.0;
			PF_CHECK(Tests, std::abs((*ExportedCounter)["ts"].get<double>() - Expected) < 0.01);
		}
		if (DurationEvent != Initial.Diagnostics.Events.end() && ExportedDuration != Trace["traceEvents"].end())
		{
			PF_CHECK(Tests, (*ExportedDuration)["ph"] == "X");
			const double Expected = static_cast<double>(DurationEvent->StartTimestampNanoseconds -
				Initial.Diagnostics.CaptureStartTimestampNanoseconds) / 1000.0;
			PF_CHECK(Tests, std::abs((*ExportedDuration)["ts"].get<double>() - Expected) < 0.01);
		}
	}

	// The service owns its active/export snapshots; later caller mutation cannot rewrite provenance or history.
	Observe.Resources.clear();
	Observe.CompletedGpuTimings.clear();
	const auto ReSerialized = EditorPerformanceCaptureService::Serialize(*Completed);
	PF_CHECK(Tests, ReSerialized && *ReSerialized == *Serialized);
	PF_CHECK(Tests, ActiveCopy && ActiveCopy->Snapshot.Diagnostics.CaptureEndTimestampNanoseconds ==
		0);
	const auto ActiveStillOpen = ActiveCopy ? EditorPerformanceCaptureService::Serialize(*ActiveCopy) :
		std::expected<std::string, EditorPerformanceExportError>(std::unexpected(EditorPerformanceExportError{}));
	PF_CHECK(Tests, ActiveStillOpen && ActiveSerialized && *ActiveStillOpen == *ActiveSerialized);
	EditorPerformanceSnapshot PostStop = MakeSnapshot(Initial.Diagnostics.SessionId, Final.Diagnostics.SnapshotTimestampNanoseconds + 10, false);
	PostStop.CompletedGpuTimings.push_back({ Initial.Diagnostics.SessionId, "late result", "VulkanRenderer", "query", 80, 5'000, 300 });
	PF_CHECK(Tests, ObserveCapture(CaptureService, PostStop, MakeMetadata("later-workload", "later-project")).has_value());
	const auto ReExportSnapshot = CaptureService.GetExportSnapshot();
	PF_CHECK(Tests, ReExportSnapshot && ReExportSnapshot->Snapshot.Diagnostics.CaptureEndTimestampNanoseconds ==
		Final.Diagnostics.CaptureEndTimestampNanoseconds);
	PF_CHECK(Tests, ReExportSnapshot && ReExportSnapshot->Metadata.Workload == "cold-open");
	PF_CHECK(Tests, ReExportSnapshot && ReExportSnapshot->Snapshot.CompletedGpuTimings.back().Name == "late result");
	PF_CHECK(Tests, ReExportSnapshot && ReExportSnapshot->Snapshot.Resources.size() == Completed->Snapshot.Resources.size());
	PF_CHECK(Tests, ReExportSnapshot && !ReExportSnapshot->Snapshot.Resources.empty() &&
		ReExportSnapshot->Snapshot.Resources.front().Label == Completed->Snapshot.Resources.front().Label);

	EditorPerformanceSnapshot LatePositive = MakeSnapshot(Initial.Diagnostics.SessionId, Final.Diagnostics.SnapshotTimestampNanoseconds + 20, false);
	LatePositive.CompletedGpuTimings.push_back({ Initial.Diagnostics.SessionId, "late positive", "VulkanRenderer", "query", 81, 5'010, 250 });
	PF_CHECK(Tests, ObserveCapture(CaptureService, LatePositive, MakeMetadata("later-workload", "later-project")).has_value());
	const auto RecoveredTimingCapture = CaptureService.GetExportSnapshot();
	PF_CHECK(Tests, RecoveredTimingCapture && RecoveredTimingCapture->Snapshot.GpuTimingResultStatus ==
		EditorGpuTimingResultStatus::Informative);
	PF_CHECK(Tests, RecoveredTimingCapture && RecoveredTimingCapture->Snapshot.GpuTimingAvailable);
	PF_CHECK(Tests, RecoveredTimingCapture && RecoveredTimingCapture->Snapshot.GpuTimingUnavailableReason.empty());

	EditorPerformanceCaptureService ZeroService;
	EditorPerformanceSnapshot ZeroStart = MakeSnapshot(700, 10'000);
	ZeroStart.GpuTimingSupported = true;
	ZeroStart.GpuTimingRequested = true;
	ZeroStart.GpuTimingEnabled = true;
	PF_CHECK(Tests, BeginCapture(ZeroService, ZeroStart, MakeMetadata("isolated-zero")).has_value());
	EditorPerformanceSnapshot ZeroEnd = MakeSnapshot(700, 10'100, false);
	ZeroEnd.GpuTimingSupported = true;
	ZeroEnd.CompletedGpuTimings.push_back({ 700, "isolated frame", "renderer", "query", 1, 10'050, 0 });
	PF_CHECK(Tests, StopCapture(ZeroService, ZeroEnd, MakeMetadata("isolated-zero")).has_value());
	const auto ZeroCapture = ZeroService.GetExportSnapshot();
	PF_CHECK(Tests, ZeroCapture.has_value());
	if (ZeroCapture)
	{
		PF_CHECK(Tests, !ZeroCapture->Snapshot.GpuTimingAvailable);
		PF_CHECK(Tests, ZeroCapture->Snapshot.GpuTimingResultStatus == EditorGpuTimingResultStatus::ZeroDurationOnly);
		PF_CHECK(Tests, ZeroCapture->Snapshot.CompletedGpuTimings.size() == 1);
		PF_CHECK(Tests, ZeroCapture->Snapshot.CompletedGpuTimings[0].Quality == EditorGpuTimingSampleQuality::ZeroDuration);
	}
	EditorPerformanceSnapshot LatePositiveAfterZero = MakeSnapshot(700, 10'120, false);
	LatePositiveAfterZero.CompletedGpuTimings.push_back({ 700, "retired query", "renderer", "query", 2, 10'110, 250 });
	PF_CHECK(Tests, ObserveCapture(ZeroService, LatePositiveAfterZero, MakeMetadata("later-drain")).has_value());
	const auto RecoveredZeroCapture = ZeroService.GetExportSnapshot();
	PF_CHECK(Tests, RecoveredZeroCapture && RecoveredZeroCapture->Snapshot.GpuTimingAvailable);
	PF_CHECK(Tests, RecoveredZeroCapture && RecoveredZeroCapture->Snapshot.GpuTimingResultStatus ==
		EditorGpuTimingResultStatus::Informative);
	PF_CHECK(Tests, RecoveredZeroCapture && RecoveredZeroCapture->Snapshot.GpuTimingUnavailableReason.empty());

	EditorPerformanceCaptureService FailedTimingService;
	EditorPerformanceSnapshot FailedStart = MakeSnapshot(750, 15'000);
	FailedStart.GpuTimingSupported = true;
	FailedStart.GpuTimingRequested = true;
	FailedStart.GpuTimingFailureReported = true;
	FailedStart.GpuTimingUnavailableReason = "Injected query initialization failure";
	PF_CHECK(Tests, BeginCapture(FailedTimingService, FailedStart, MakeMetadata("failed-timing")).has_value());
	EditorPerformanceSnapshot FailedStop = MakeSnapshot(750, 15'010, false);
	FailedStop.GpuTimingSupported = true;
	FailedStop.GpuTimingRequested = true;
	FailedStop.GpuTimingFailureReported = true;
	FailedStop.GpuTimingUnavailableReason = FailedStart.GpuTimingUnavailableReason;
	PF_CHECK(Tests, StopCapture(FailedTimingService, FailedStop, MakeMetadata("failed-timing")).has_value());
	const auto FailedCapture = FailedTimingService.GetExportSnapshot();
	PF_CHECK(Tests, FailedCapture && FailedCapture->Snapshot.GpuTimingResultStatus == EditorGpuTimingResultStatus::Failed);
	PF_CHECK(Tests, FailedCapture && FailedCapture->Snapshot.GpuTimingFailureReported);

	EditorPerformanceCaptureService StaleService;
	EditorPerformanceSnapshot StaleStart = MakeSnapshot(800, 20'000);
	PF_CHECK(Tests, BeginCapture(StaleService, StaleStart, MakeMetadata("stale-session")).has_value());
	EditorPerformanceSnapshot StaleObserve = MakeSnapshot(800, 20'010);
	StaleObserve.CompletedGpuTimings.push_back({ 799, "old session", "renderer", "query", 3, 20'005, 20 });
	PF_CHECK(Tests, ObserveCapture(StaleService, StaleObserve, MakeMetadata("stale-session")).has_value());
	EditorPerformanceSnapshot StaleStop = MakeSnapshot(800, 20'020, false);
	PF_CHECK(Tests, StopCapture(StaleService, StaleStop, MakeMetadata("stale-session")).has_value());
	const auto StaleCapture = StaleService.GetExportSnapshot();
	PF_CHECK(Tests, StaleCapture && StaleCapture->Snapshot.Timing.StaleSessionDiscardCount == 1);
	PF_CHECK(Tests, StaleCapture && StaleCapture->Snapshot.CompletedGpuTimings.empty());
	PF_CHECK(Tests, StaleCapture && StaleCapture->Snapshot.GpuTimingResultStatus == EditorGpuTimingResultStatus::NoSamples);

	EditorPerformanceCaptureService SegmentService;
	EditorPerformanceSnapshot SegmentStart = MakeSnapshot(900, 30'000);
	for (uint64_t Index = 0; Index < 40; ++Index)
		SegmentStart.Resources.push_back({ "metric-" + std::to_string(Index), "test", "scope", Index });
	PF_CHECK(Tests, BeginCapture(SegmentService, SegmentStart, MakeMetadata("segment-0")).has_value());
	for (uint64_t Index = 1; Index <= 80; ++Index)
	{
		EditorPerformanceSnapshot Segment = MakeSnapshot(900, 30'000 + Index);
		Segment.Resources.push_back({ "metric", "test", "scope", Index });
		PF_CHECK(Tests, ObserveCapture(SegmentService, Segment, MakeMetadata("segment-" + std::to_string(Index))).has_value());
	}
	EditorPerformanceSnapshot SegmentStop = MakeSnapshot(900, 30'100, false);
	SegmentStop.Resources.push_back({ "metric", "test", "scope", 80 });
	PF_CHECK(Tests, StopCapture(SegmentService, SegmentStop, MakeMetadata("segment-80")).has_value());
	const auto Segments = SegmentService.GetExportSnapshot();
	PF_CHECK(Tests, Segments && Segments->ResourceSegments.size() == 64);
	PF_CHECK(Tests, Segments && Segments->Snapshot.ResourceSegmentHistoryEvictionCount == 17);
	PF_CHECK(Tests, Segments && Segments->Snapshot.ResourceSegmentMetricTruncationCount == 8);

	EditorPerformanceCaptureService StopBoundaryService;
	EditorPerformanceSnapshot StopBoundaryStart = MakeSnapshot(950, 40'000);
	StopBoundaryStart.Resources.push_back({ "loaded assets", "catalog", "count", 2 });
	PF_CHECK(Tests, BeginCapture(StopBoundaryService, StopBoundaryStart, MakeMetadata("boundary-start")).has_value());
	EditorPerformanceSnapshot StopBoundaryStop = MakeSnapshot(950, 40'200, false);
	StopBoundaryStop.Diagnostics.CaptureEndTimestampNanoseconds = 40'100;
	StopBoundaryStop.Resources.push_back({ "loaded assets", "catalog", "count", 3 });
	PF_CHECK(Tests, StopCapture(StopBoundaryService, StopBoundaryStop, MakeMetadata("boundary-stop")).has_value());
	const auto BoundedStopCapture = StopBoundaryService.GetExportSnapshot();
	PF_CHECK(Tests, BoundedStopCapture && BoundedStopCapture->ResourceSegments.back().TimestampNanoseconds == 40'100);
	PF_CHECK(Tests, BoundedStopCapture && BoundedStopCapture->ResourceSegments.back().Workload == "boundary-stop");
	PF_CHECK(Tests, Segments && Segments->ResourceSegments.front().Workload == "segment-17");
	PF_CHECK(Tests, Segments && Segments->ResourceSegments.back().Workload == "segment-80");
	PF_CHECK(Tests, Segments && Segments->ResourceSegments.front().Resources.size() == 1);

	EditorPerformanceCaptureService MetricService;
	EditorPerformanceSnapshot ManyMetrics = MakeSnapshot(950, 35'000);
	for (uint64_t Index = 0; Index < 40; ++Index)
		ManyMetrics.Resources.push_back({ "metric-" + std::to_string(Index), "test", "scope", Index });
	PF_CHECK(Tests, BeginCapture(MetricService, ManyMetrics, MakeMetadata("many-metrics")).has_value());
	EditorPerformanceSnapshot ManyMetricsStop = MakeSnapshot(950, 35'010, false);
	ManyMetricsStop.Resources = ManyMetrics.Resources;
	PF_CHECK(Tests, StopCapture(MetricService, ManyMetricsStop, MakeMetadata("many-metrics")).has_value());
	const auto MetricCapture = MetricService.GetExportSnapshot();
	PF_CHECK(Tests, MetricCapture && MetricCapture->ResourceSegments.size() == 1);
	PF_CHECK(Tests, MetricCapture && MetricCapture->ResourceSegments.front().Resources.size() == 32);
	PF_CHECK(Tests, MetricCapture && MetricCapture->Snapshot.ResourceSegmentMetricTruncationCount == 16);
	const auto MetricSerialized = MetricCapture ? EditorPerformanceCaptureService::Serialize(*MetricCapture) :
		std::expected<std::string, EditorPerformanceExportError>(std::unexpected(EditorPerformanceExportError{}));
	PF_CHECK(Tests, MetricSerialized.has_value());
	if (MetricSerialized)
	{
		const nlohmann::json MetricTrace = nlohmann::json::parse(*MetricSerialized);
		PF_CHECK(Tests, MetricTrace["metadata"]["resource_segments"][0]["resources"].size() == 32);
		PF_CHECK(Tests, MetricTrace["metadata"]["resource_segment_metric_truncation_count"] == 16);
	}

	const std::filesystem::path ExportFailureRoot = std::filesystem::temp_directory_path() /
		("PulseForgeTraceExportFailure-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	std::error_code FileError;
	const bool CreatedRoot = std::filesystem::create_directory(ExportFailureRoot, FileError);
	PF_CHECK(Tests, CreatedRoot && !FileError);
	if (CreatedRoot)
	{
		const std::filesystem::path ExistingDestination = ExportFailureRoot / "existing-capture.json";
		std::ofstream ExistingContent(ExistingDestination, std::ios::binary | std::ios::trunc);
		ExistingContent << "preserve existing bytes";
		ExistingContent.close();
		const std::filesystem::path BlockedDestination = ExportFailureRoot / "blocked" / "capture.json";
		const auto FailedWrite = EditorPerformanceCaptureService::ExportAtomically(BlockedDestination, *Completed);
		PF_CHECK(Tests, !FailedWrite);
		PF_CHECK(Tests, !FailedWrite && FailedWrite.error().Code == EditorPerformanceExportErrorCode::TemporaryWriteFailed);
		{
			std::ifstream Preserved(ExistingDestination, std::ios::binary);
			PF_CHECK(Tests, std::string((std::istreambuf_iterator<char>(Preserved)), {}) == "preserve existing bytes");
		}
		const std::filesystem::path ExistingDirectory = ExportFailureRoot / "existing-directory.json";
		FileError.clear();
		std::filesystem::create_directory(ExistingDirectory, FileError);
		PF_CHECK(Tests, !FileError);
		const std::filesystem::path Sentinel = ExistingDirectory / "sentinel.txt";
		std::ofstream SentinelFile(Sentinel, std::ios::binary | std::ios::trunc);
		SentinelFile << "preserve on atomic replace failure";
		SentinelFile.close();
		const auto FailedReplace = EditorPerformanceCaptureService::ExportAtomically(ExistingDirectory, *Completed);
		PF_CHECK(Tests, !FailedReplace && FailedReplace.error().Code == EditorPerformanceExportErrorCode::AtomicReplaceFailed);
		{
			std::ifstream SentinelAfterFailure(Sentinel, std::ios::binary);
			PF_CHECK(Tests, std::string((std::istreambuf_iterator<char>(SentinelAfterFailure)), {}) ==
				"preserve on atomic replace failure");
		}
		const auto Retry = EditorPerformanceCaptureService::ExportAtomically(ExistingDestination, *Completed);
		PF_CHECK(Tests, Retry.has_value());
		std::string ExportedText;
		{
			std::ifstream Exported(ExistingDestination, std::ios::binary);
			ExportedText.assign(std::istreambuf_iterator<char>(Exported), {});
		}
		PF_CHECK(Tests, ExportedText == *Serialized);
		const auto ReplaceFailure = EditorPerformanceCaptureService::ExportAtomically(ExportFailureRoot, *Completed);
		PF_CHECK(Tests, !ReplaceFailure && ReplaceFailure.error().Code == EditorPerformanceExportErrorCode::AtomicReplaceFailed);
		PF_CHECK(Tests, std::filesystem::is_regular_file(ExistingDestination));
		{
			std::ifstream AfterReplaceFailure(ExistingDestination, std::ios::binary);
			PF_CHECK(Tests, std::string((std::istreambuf_iterator<char>(AfterReplaceFailure)), {}) == *Serialized);
		}
		std::filesystem::remove_all(ExportFailureRoot, FileError);
		PF_CHECK(Tests, !FileError);
	}

	EditorPerformanceCaptureService ResetService;
	EditorPerformanceSnapshot FirstSession = MakeSnapshot(1000, 40'000);
	PF_CHECK(Tests, BeginCapture(ResetService, FirstSession, MakeMetadata("first-session")).has_value());
	EditorPerformanceSnapshot FirstStop = MakeSnapshot(1000, 40'010, false);
	PF_CHECK(Tests, StopCapture(ResetService, FirstStop, MakeMetadata("first-session")).has_value());
	EditorPerformanceSnapshot SecondSession = MakeSnapshot(1001, 50'000);
	PF_CHECK(Tests, BeginCapture(ResetService, SecondSession, MakeMetadata("second-session")).has_value());
	EditorPerformanceSnapshot SecondObserve = MakeSnapshot(1001, 50'010);
	SecondObserve.CompletedGpuTimings.push_back({ 1000, "retired session", "renderer", "query", 9, 50'001, 400 });
	SecondObserve.CompletedGpuTimings.push_back({ 1001, "current session", "renderer", "query", 10, 50'002, 500 });
	PF_CHECK(Tests, ObserveCapture(ResetService, SecondObserve, MakeMetadata("second-session"),
		{ .RendererGeneration = 1, .QueueOverwriteCount = 30, .InvalidResultCount = 7 }).has_value());
	EditorPerformanceSnapshot SecondStop = MakeSnapshot(1001, 50'020, false);
	PF_CHECK(Tests, StopCapture(ResetService, SecondStop, MakeMetadata("second-session"),
		{ .RendererGeneration = 1, .QueueOverwriteCount = 30, .InvalidResultCount = 7 }).has_value());
	const auto ResetCapture = ResetService.GetExportSnapshot();
	PF_CHECK(Tests, ResetCapture && ResetCapture->Snapshot.Diagnostics.SessionId == 1001);
	PF_CHECK(Tests, ResetCapture && ResetCapture->Snapshot.Timing.RendererQueueOverwriteCount == 30);
	PF_CHECK(Tests, ResetCapture && ResetCapture->Snapshot.Timing.InvalidResultCount == 7);
	PF_CHECK(Tests, ResetCapture && ResetCapture->Snapshot.Timing.StaleSessionDiscardCount == 1);
	PF_CHECK(Tests, ResetCapture && ResetCapture->Snapshot.Timing.CaptureHistoryEvictionCount == 0);
	PF_CHECK(Tests, ResetCapture && ResetCapture->Snapshot.CompletedGpuTimings.size() == 1);
	PF_CHECK(Tests, ResetCapture && ResetCapture->Snapshot.CompletedGpuTimings.front().SessionId == 1001);

	EditorPerformanceCaptureService ShortHistoryService;
	EditorPerformanceSnapshot ShortStart = MakeSnapshot(1100, 60'000);
	PF_CHECK(Tests, BeginCapture(ShortHistoryService, ShortStart, MakeMetadata("short-history")).has_value());
	EditorPerformanceSnapshot ShortObserve = MakeSnapshot(1100, 60'010);
	for (uint64_t Index = 0; Index < 63; ++Index)
		ShortObserve.CompletedGpuTimings.push_back({ 1100, "frame", "renderer", "query", Index, 60'001 + Index, 1 });
	PF_CHECK(Tests, ObserveCapture(ShortHistoryService, ShortObserve, MakeMetadata("short-history")).has_value());
	EditorPerformanceSnapshot ShortStop = MakeSnapshot(1100, 60'020, false);
	PF_CHECK(Tests, StopCapture(ShortHistoryService, ShortStop, MakeMetadata("short-history")).has_value());
	const auto ShortCapture = ShortHistoryService.GetExportSnapshot();
	PF_CHECK(Tests, ShortCapture && ShortCapture->Snapshot.CompletedGpuTimings.size() == 63);
	PF_CHECK(Tests, ShortCapture && ShortCapture->Snapshot.Timing.CaptureHistoryEvictionCount == 0);

	EditorPerformanceCaptureService ExactHistoryService;
	EditorPerformanceSnapshot ExactStart = MakeSnapshot(1200, 70'000);
	PF_CHECK(Tests, BeginCapture(ExactHistoryService, ExactStart, MakeMetadata("exact-history")).has_value());
	EditorPerformanceSnapshot ExactObserve = MakeSnapshot(1200, 70'010);
	for (uint64_t Index = 0; Index < 64; ++Index)
		ExactObserve.CompletedGpuTimings.push_back({ 1200, "frame", "renderer", "query", Index, 70'001 + Index, 1 });
	PF_CHECK(Tests, ObserveCapture(ExactHistoryService, ExactObserve, MakeMetadata("exact-history"),
		{ .RendererGeneration = 1, .QueueOverwriteCount = 5 }).has_value());
	EditorPerformanceSnapshot ExactStop = MakeSnapshot(1200, 70'020, false);
	PF_CHECK(Tests, StopCapture(ExactHistoryService, ExactStop, MakeMetadata("exact-history"),
		{ .RendererGeneration = 1, .QueueOverwriteCount = 5 }).has_value());
	const auto ExactCapture = ExactHistoryService.GetExportSnapshot();
	PF_CHECK(Tests, ExactCapture && ExactCapture->Snapshot.CompletedGpuTimings.size() == 64);
	PF_CHECK(Tests, ExactCapture && ExactCapture->Snapshot.Timing.CaptureHistoryEvictionCount == 0);
	PF_CHECK(Tests, ExactCapture && ExactCapture->Snapshot.Timing.RendererQueueOverwriteCount == 5);

	EditorPerformanceCaptureService CombinedLossService;
	EditorPerformanceSnapshot CombinedStart = MakeSnapshot(1250, 75'000);
	PF_CHECK(Tests, BeginCapture(CombinedLossService, CombinedStart, MakeMetadata("combined-loss")).has_value());
	EditorPerformanceSnapshot CombinedObserve = MakeSnapshot(1250, 75'010);
	for (uint64_t Index = 0; Index < 80; ++Index)
		CombinedObserve.CompletedGpuTimings.push_back({ 1250, "frame", "renderer", "query", Index, 75'001 + Index, 1 });
	PF_CHECK(Tests, ObserveCapture(CombinedLossService, CombinedObserve, MakeMetadata("combined-loss"),
		{ .RendererGeneration = 1, .QueueOverwriteCount = 5 }).has_value());
	EditorPerformanceSnapshot CombinedStop = MakeSnapshot(1250, 75'020, false);
	PF_CHECK(Tests, StopCapture(CombinedLossService, CombinedStop, MakeMetadata("combined-loss"),
		{ .RendererGeneration = 1, .QueueOverwriteCount = 5 }).has_value());
	const auto CombinedCapture = CombinedLossService.GetExportSnapshot();
	PF_CHECK(Tests, CombinedCapture && CombinedCapture->Snapshot.Timing.CaptureHistoryEvictionCount == 16);
	PF_CHECK(Tests, CombinedCapture && CombinedCapture->Snapshot.Timing.RendererQueueOverwriteCount == 5);
	PF_CHECK(Tests, CombinedCapture && CombinedCapture->Snapshot.CompletedGpuTimings.size() == 64);

	EditorPerformanceCaptureService InitialStaleService;
	EditorPerformanceSnapshot InitialStale = MakeSnapshot(1300, 80'000);
	InitialStale.CompletedGpuTimings.push_back({ 1299, "retired result", "renderer", "query", 5, 79'999, 10 });
	PF_CHECK(Tests, BeginCapture(InitialStaleService, InitialStale, MakeMetadata("initial-stale")).has_value());
	EditorPerformanceSnapshot InitialStaleStop = MakeSnapshot(1300, 80'010, false);
	PF_CHECK(Tests, StopCapture(InitialStaleService, InitialStaleStop, MakeMetadata("initial-stale")).has_value());
	const auto InitialStaleCapture = InitialStaleService.GetExportSnapshot();
	PF_CHECK(Tests, InitialStaleCapture && InitialStaleCapture->Snapshot.Timing.StaleSessionDiscardCount == 1);
	PF_CHECK(Tests, InitialStaleCapture && InitialStaleCapture->Snapshot.CompletedGpuTimings.empty());

	EditorPerformanceCaptureService OutOfOrderService;
	EditorPerformanceSnapshot OutOfOrderStart = MakeSnapshot(1350, 85'000);
	PF_CHECK(Tests, BeginCapture(OutOfOrderService, OutOfOrderStart, MakeMetadata("out-of-order")).has_value());
	RendererTimingObservation LaterFrame = MakeRendererObservation(OutOfOrderStart);
	LaterFrame.NewlyCompletedSamples.push_back({ 1350, "frame", "renderer", "query", 2, 85'010, 20 });
	PF_CHECK(Tests, OutOfOrderService.Observe(LaterFrame, 85'010).has_value());
	RendererTimingObservation EarlierFrame = MakeRendererObservation(OutOfOrderStart);
	EarlierFrame.NewlyCompletedSamples.push_back({ 1350, "frame", "renderer", "query", 1, 85'011, 10 });
	PF_CHECK(Tests, OutOfOrderService.Observe(EarlierFrame, 85'011).has_value());
	const auto OrderedCapture = OutOfOrderService.GetExportSnapshot();
	PF_CHECK(Tests, OrderedCapture && OrderedCapture->Snapshot.CompletedGpuTimings.size() == 2);
	PF_CHECK(Tests, OrderedCapture && OrderedCapture->Snapshot.CompletedGpuTimings.front().FrameSequence == 1);
	PF_CHECK(Tests, OrderedCapture && OrderedCapture->Snapshot.CompletedGpuTimings.back().FrameSequence == 2);

	EditorPerformanceCaptureService CounterLifecycle;
	EditorPerformanceSnapshot CounterStart = MakeSnapshot(1400, 90'000);
	PF_CHECK(Tests, BeginCapture(CounterLifecycle, CounterStart, MakeMetadata("counter-lifecycle"),
		{ .RendererGeneration = 1, .QueueOverwriteCount = 100, .InvalidResultCount = 10 }).has_value());
	EditorPerformanceSnapshot CounterAdvance = MakeSnapshot(1400, 90'010);
	PF_CHECK(Tests, ObserveCapture(CounterLifecycle, CounterAdvance, MakeMetadata("counter-lifecycle"),
		{ .RendererGeneration = 1, .QueueOverwriteCount = 103, .InvalidResultCount = 12 }).has_value());
	EditorPerformanceSnapshot CounterStop = MakeSnapshot(1400, 90'020, false);
	PF_CHECK(Tests, StopCapture(CounterLifecycle, CounterStop, MakeMetadata("counter-lifecycle"),
		{ .RendererGeneration = 1, .QueueOverwriteCount = 103, .InvalidResultCount = 12 }).has_value());
	for (uint64_t Index = 0; Index < 4; ++Index)
	{
		const auto RepeatedObservation = MakeRendererObservation(CounterStop,
			{ .RendererGeneration = 1, .QueueOverwriteCount = 103, .InvalidResultCount = 12 });
		PF_CHECK(Tests, CounterLifecycle.Observe(RepeatedObservation, 90'021 + Index).has_value());
		const auto RepeatedExport = CounterLifecycle.GetExportSnapshot();
		PF_CHECK(Tests, RepeatedExport && RepeatedExport->Snapshot.Timing.RendererQueueOverwriteCount == 3);
		PF_CHECK(Tests, RepeatedExport && RepeatedExport->Snapshot.Timing.InvalidResultCount == 2);
	}
	const auto OneNewLoss = MakeRendererObservation(CounterStop,
		{ .RendererGeneration = 1, .QueueOverwriteCount = 104, .InvalidResultCount = 12 });
	PF_CHECK(Tests, CounterLifecycle.Observe(OneNewLoss, 90'030).has_value());
	const auto CounterResult = CounterLifecycle.GetExportSnapshot();
	PF_CHECK(Tests, CounterResult && CounterResult->Snapshot.Timing.RendererQueueOverwriteCount == 4);
	PF_CHECK(Tests, CounterResult && CounterResult->Snapshot.Timing.InvalidResultCount == 2);
	EditorPerformanceSnapshot RestartedCapture = MakeSnapshot(1401, 91'000);
	PF_CHECK(Tests, BeginCapture(CounterLifecycle, RestartedCapture, MakeMetadata("restarted"),
		{ .RendererGeneration = 1, .QueueOverwriteCount = 200, .InvalidResultCount = 30 }).has_value());
	EditorPerformanceSnapshot RestartedObservation = MakeSnapshot(1401, 91'010);
	PF_CHECK(Tests, ObserveCapture(CounterLifecycle, RestartedObservation, MakeMetadata("restarted"),
		{ .RendererGeneration = 1, .QueueOverwriteCount = 201, .InvalidResultCount = 31 }).has_value());
	EditorPerformanceSnapshot RestartedStop = MakeSnapshot(1401, 91'020, false);
	PF_CHECK(Tests, StopCapture(CounterLifecycle, RestartedStop, MakeMetadata("restarted"),
		{ .RendererGeneration = 1, .QueueOverwriteCount = 201, .InvalidResultCount = 31 }).has_value());
	const auto RestartedResult = CounterLifecycle.GetExportSnapshot();
	PF_CHECK(Tests, RestartedResult && RestartedResult->Snapshot.Timing.RendererQueueOverwriteCount == 1);
	PF_CHECK(Tests, RestartedResult && RestartedResult->Snapshot.Timing.InvalidResultCount == 1);

	EditorPerformanceCaptureService GenerationLifecycle;
	EditorPerformanceSnapshot GenerationStart = MakeSnapshot(1450, 92'000);
	PF_CHECK(Tests, BeginCapture(GenerationLifecycle, GenerationStart, MakeMetadata("generation"),
		{ .RendererGeneration = 1, .QueueOverwriteCount = 9, .InvalidResultCount = 4 }).has_value());
	const auto NewGeneration = MakeRendererObservation(GenerationStart,
		{ .RendererGeneration = 2, .QueueOverwriteCount = 0, .InvalidResultCount = 0 });
	PF_CHECK(Tests, GenerationLifecycle.Observe(NewGeneration, 92'010).has_value());
	const auto GenerationIncrement = MakeRendererObservation(GenerationStart,
		{ .RendererGeneration = 2, .QueueOverwriteCount = 2, .InvalidResultCount = 1 });
	PF_CHECK(Tests, GenerationLifecycle.Observe(GenerationIncrement, 92'020).has_value());
	const auto GenerationRegression = MakeRendererObservation(GenerationStart,
		{ .RendererGeneration = 2, .QueueOverwriteCount = 1, .InvalidResultCount = 1 });
	PF_CHECK(Tests, GenerationLifecycle.Observe(GenerationRegression, 92'030).has_value());
	const auto GenerationRecovery = MakeRendererObservation(GenerationStart,
		{ .RendererGeneration = 2, .QueueOverwriteCount = 4, .InvalidResultCount = 2 });
	PF_CHECK(Tests, GenerationLifecycle.Observe(GenerationRecovery, 92'040).has_value());
	const auto GenerationResult = GenerationLifecycle.GetExportSnapshot();
	PF_CHECK(Tests, GenerationResult && GenerationResult->Snapshot.Timing.RendererGenerationChangeCount == 1);
	PF_CHECK(Tests, GenerationResult && GenerationResult->Snapshot.Timing.CounterRegressionCount == 1);
	PF_CHECK(Tests, GenerationResult && GenerationResult->Snapshot.Timing.RendererQueueOverwriteCount == 4);
	PF_CHECK(Tests, GenerationResult && GenerationResult->Snapshot.Timing.InvalidResultCount == 2);

	EditorPerformanceCaptureService HiddenCollection;
	uint64_t IdleLifecycleUpdates = 0;
	for (uint64_t Frame = 0; Frame < 2'000; ++Frame)
		if (HiddenCollection.NeedsLifecycleUpdate()) ++IdleLifecycleUpdates;
	PF_CHECK(Tests, IdleLifecycleUpdates == 0);
	EditorPerformanceSnapshot HiddenStart = MakeSnapshot(1500, 100'000);
	HiddenStart.Diagnostics.EventCapacity = PulseForge::DiagnosticRecorder::MaximumCapacity;
	HiddenStart.Diagnostics.Events.reserve(PulseForge::DiagnosticRecorder::MaximumCapacity);
	for (size_t Index = 0; Index < PulseForge::DiagnosticRecorder::MaximumCapacity; ++Index)
	{
		PulseForge::DiagnosticEvent Event;
		Event.SessionId = 1500;
		Event.Sequence = Index + 1;
		Event.Name = "bounded retained event";
		HiddenStart.Diagnostics.Events.push_back(std::move(Event));
	}
	HiddenStart.Resources.push_back({ "Asset A", "test", "capture-time observation", 1,
		EditorResourceMetricKind::Count, EditorResourceMetricQuality::ExactPayload });
	PF_CHECK(Tests, BeginCapture(HiddenCollection, HiddenStart, MakeMetadata("hidden-collection"),
		{ .RendererGeneration = 1 }).has_value());
	EditorPerformanceSnapshot HiddenStop = HiddenStart;
	HiddenStop.Diagnostics.CaptureEnabled = false;
	HiddenStop.Diagnostics.CaptureEndTimestampNanoseconds = 100'010;
	HiddenStop.Diagnostics.SnapshotTimestampNanoseconds = 100'010;
	const auto PendingAtStop = MakeRendererObservation(HiddenStop, { .RendererGeneration = 1 });
	auto PendingObservation = PendingAtStop;
	PendingObservation.CollectionOutstanding = true;
	PF_CHECK(Tests, HiddenCollection.Stop(HiddenStop.Diagnostics, PendingObservation, HiddenStart.Resources,
		MakeMetadata("hidden-collection")).has_value());
	PF_CHECK(Tests, !HiddenCollection.IsCollectionSettled());
	PF_CHECK(Tests, HiddenCollection.NeedsLifecycleUpdate());
	const uint64_t MaterializationsBeforeUpdates = HiddenCollection.GetFullSnapshotMaterializationCount();
	uint64_t PendingCollectionUpdates = 0;
	for (uint64_t Index = 0; Index < 1'000; ++Index)
	{
		if (!HiddenCollection.NeedsLifecycleUpdate()) continue;
		++PendingCollectionUpdates;
		auto Delayed = MakeRendererObservation(HiddenStop, { .RendererGeneration = 1 });
		Delayed.CollectionOutstanding = Index < 2;
		if (Index == 0)
			Delayed.NewlyCompletedSamples.push_back({ 1500, "frame", "renderer", "query", 4, 100'011, 250 });
		const std::vector<EditorResourceMetric> ChangedLiveResources{ { "Project B", "test", "post-stop live metrics", 99,
			EditorResourceMetricKind::Count, EditorResourceMetricQuality::ExactPayload } };
		const auto ChangedMetadata = MakeMetadata("project-b", "project-b-fingerprint");
		PF_CHECK(Tests, HiddenCollection.Observe(Delayed, 100'011 + Index, &ChangedLiveResources, &ChangedMetadata).has_value());
	}
	PF_CHECK(Tests, PendingCollectionUpdates == 3);
	PF_CHECK(Tests, HiddenCollection.IsCollectionSettled());
	PF_CHECK(Tests, !HiddenCollection.NeedsLifecycleUpdate());
	PF_CHECK(Tests, HiddenCollection.GetFullSnapshotMaterializationCount() == MaterializationsBeforeUpdates);
	const auto* RetainedPresentation = HiddenCollection.GetPresentationSnapshot();
	PF_CHECK(Tests, RetainedPresentation && RetainedPresentation->Diagnostics.Events.size() == PulseForge::DiagnosticRecorder::MaximumCapacity);
	PF_CHECK(Tests, RetainedPresentation && RetainedPresentation->CompletedGpuTimings.size() == 1);
	PF_CHECK(Tests, RetainedPresentation && RetainedPresentation->Resources.size() == 1);
	PF_CHECK(Tests, RetainedPresentation && RetainedPresentation->Resources.front().Label == "Asset A");
	const auto RetainedExport = HiddenCollection.GetExportSnapshot();
	PF_CHECK(Tests, RetainedExport && RetainedExport->ResourceSegments.front().Workload == "hidden-collection");
	PF_CHECK(Tests, RetainedExport && RetainedExport->ResourceSegments.front().ContentFingerprint == "project-a");
	PF_CHECK(Tests, HiddenCollection.GetFullSnapshotMaterializationCount() == MaterializationsBeforeUpdates);

	EditorPerformanceCaptureService ActiveExportLifecycle;
	EditorPerformanceSnapshot ActiveExportStart = MakeSnapshot(1600, 110'000);
	PF_CHECK(Tests, BeginCapture(ActiveExportLifecycle, ActiveExportStart, MakeMetadata("active-export")).has_value());
	PulseForge::DiagnosticSnapshot ActivePoint = ActiveExportStart.Diagnostics;
	ActivePoint.SnapshotTimestampNanoseconds = 110'010;
	const auto ActivePointExport = ActiveExportLifecycle.GetExportSnapshot(&ActivePoint);
	PF_CHECK(Tests, ActivePointExport && ActivePointExport->Snapshot.Diagnostics.CaptureEndTimestampNanoseconds == 0);
	PF_CHECK(Tests, ActiveExportLifecycle.IsActive());
	const auto AfterExportObservation = MakeRendererObservation(ActiveExportStart);
	PF_CHECK(Tests, ActiveExportLifecycle.Observe(AfterExportObservation, 110'020).has_value());
	PF_CHECK(Tests, ActiveExportLifecycle.IsActive());
}
