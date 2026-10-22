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
	auto BeginResult = CaptureService.Begin(Initial, MakeMetadata("cold-open"));
	PF_CHECK(Tests, BeginResult.has_value());
	PF_CHECK(Tests, !CaptureService.Begin(Initial, MakeMetadata("duplicate")));
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
	Observe.GpuTimingRendererOverwriteCount = 0;
	Observe.GpuTimingInvalidResultCount = 2;
	for (uint64_t Index = 0; Index < 80; ++Index)
	{
		Observe.CompletedGpuTimings.push_back({ Observe.Diagnostics.SessionId, "frame", "VulkanRenderer", "query",
			Index, 2'000 + Index, Index == 0 ? 0 : 100 + Index });
	}
	auto ObserveResult = CaptureService.Observe(Observe, MakeMetadata("warm-open", "project-b"));
	PF_CHECK(Tests, ObserveResult.has_value());
	const auto WrongSession = CaptureService.Observe(MakeSnapshot(Initial.Diagnostics.SessionId + 99, 3'000), MakeMetadata("stale"));
	PF_CHECK(Tests, !WrongSession && WrongSession.error().Code == EditorPerformanceCaptureErrorCode::StaleSession);

	EditorPerformanceSnapshot Final = MakeSnapshot(Initial.Diagnostics.SessionId, Observe.Diagnostics.SnapshotTimestampNanoseconds + 10, false);
	Final.Diagnostics.CaptureStartTimestampNanoseconds = Initial.Diagnostics.CaptureStartTimestampNanoseconds;
	Final.Diagnostics.Events = Observe.Diagnostics.Events;
	Final.Resources = Observe.Resources;
	Final.GpuTimingSupported = true;
	Final.GpuTimingRequested = true;
	Final.GpuTimingEnabled = false;
	Final.GpuTimingRendererOverwriteCount = 0;
	Final.GpuTimingInvalidResultCount = 2;
	PF_CHECK(Tests, CaptureService.Stop(Final, MakeMetadata("warm-open", "project-b")).has_value());
	PF_CHECK(Tests, !CaptureService.IsActive());
	const auto Completed = CaptureService.GetExportSnapshot();
	PF_CHECK(Tests, Completed.has_value());
	if (!Completed) return;
	PF_CHECK(Tests, Completed->Snapshot.GpuTimingResultStatus == EditorGpuTimingResultStatus::Informative);
	PF_CHECK(Tests, Completed->Snapshot.GpuTimingAvailable);
	PF_CHECK(Tests, Completed->Snapshot.GpuTimingCaptureHistoryEvictionCount == 16);
	PF_CHECK(Tests, Completed->Snapshot.GpuTimingRendererOverwriteCount == 0);
	PF_CHECK(Tests, Completed->Snapshot.GpuTimingInvalidResultCount == 2);
	PF_CHECK(Tests, Completed->Snapshot.GpuTimingStaleSessionDiscardCount == 0);
	PF_CHECK(Tests, Completed->Snapshot.GpuTimingZeroDurationCount == 1);
	PF_CHECK(Tests, Completed->Snapshot.CompletedGpuTimings.size() == 64);
	PF_CHECK(Tests, Completed->Snapshot.CompletedGpuTimings.front().FrameSequence == 16);
	PF_CHECK(Tests, Completed->Snapshot.CompletedGpuTimings.back().FrameSequence == 79);
	PF_CHECK(Tests, Completed->ResourceSegments.size() == 2);
	PF_CHECK(Tests, Completed->ResourceSegments.front().Workload == "cold-open");
	PF_CHECK(Tests, Completed->ResourceSegments.back().Workload == "warm-open");
	PF_CHECK(Tests, Completed->ResourceSegments.back().ContentFingerprint == "project-b");
	PF_CHECK(Tests, Completed->Snapshot.GpuTimingPositiveDurationCount == 79);

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
	PF_CHECK(Tests, CaptureService.Observe(PostStop, MakeMetadata("later-workload", "later-project")).has_value());
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
	PF_CHECK(Tests, CaptureService.Observe(LatePositive, MakeMetadata("later-workload", "later-project")).has_value());
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
	PF_CHECK(Tests, ZeroService.Begin(ZeroStart, MakeMetadata("isolated-zero")).has_value());
	EditorPerformanceSnapshot ZeroEnd = MakeSnapshot(700, 10'100, false);
	ZeroEnd.GpuTimingSupported = true;
	ZeroEnd.CompletedGpuTimings.push_back({ 700, "isolated frame", "renderer", "query", 1, 10'050, 0 });
	PF_CHECK(Tests, ZeroService.Stop(ZeroEnd, MakeMetadata("isolated-zero")).has_value());
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
	PF_CHECK(Tests, ZeroService.Observe(LatePositiveAfterZero, MakeMetadata("later-drain")).has_value());
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
	PF_CHECK(Tests, FailedTimingService.Begin(FailedStart, MakeMetadata("failed-timing")).has_value());
	EditorPerformanceSnapshot FailedStop = MakeSnapshot(750, 15'010, false);
	FailedStop.GpuTimingSupported = true;
	FailedStop.GpuTimingRequested = true;
	FailedStop.GpuTimingFailureReported = true;
	FailedStop.GpuTimingUnavailableReason = FailedStart.GpuTimingUnavailableReason;
	PF_CHECK(Tests, FailedTimingService.Stop(FailedStop, MakeMetadata("failed-timing")).has_value());
	const auto FailedCapture = FailedTimingService.GetExportSnapshot();
	PF_CHECK(Tests, FailedCapture && FailedCapture->Snapshot.GpuTimingResultStatus == EditorGpuTimingResultStatus::Failed);
	PF_CHECK(Tests, FailedCapture && FailedCapture->Snapshot.GpuTimingFailureReported);

	EditorPerformanceCaptureService StaleService;
	EditorPerformanceSnapshot StaleStart = MakeSnapshot(800, 20'000);
	PF_CHECK(Tests, StaleService.Begin(StaleStart, MakeMetadata("stale-session")).has_value());
	EditorPerformanceSnapshot StaleObserve = MakeSnapshot(800, 20'010);
	StaleObserve.CompletedGpuTimings.push_back({ 799, "old session", "renderer", "query", 3, 20'005, 20 });
	PF_CHECK(Tests, StaleService.Observe(StaleObserve, MakeMetadata("stale-session")).has_value());
	EditorPerformanceSnapshot StaleStop = MakeSnapshot(800, 20'020, false);
	PF_CHECK(Tests, StaleService.Stop(StaleStop, MakeMetadata("stale-session")).has_value());
	const auto StaleCapture = StaleService.GetExportSnapshot();
	PF_CHECK(Tests, StaleCapture && StaleCapture->Snapshot.GpuTimingStaleSessionDiscardCount == 1);
	PF_CHECK(Tests, StaleCapture && StaleCapture->Snapshot.CompletedGpuTimings.empty());
	PF_CHECK(Tests, StaleCapture && StaleCapture->Snapshot.GpuTimingResultStatus == EditorGpuTimingResultStatus::NoSamples);

	EditorPerformanceCaptureService SegmentService;
	EditorPerformanceSnapshot SegmentStart = MakeSnapshot(900, 30'000);
	for (uint64_t Index = 0; Index < 40; ++Index)
		SegmentStart.Resources.push_back({ "metric-" + std::to_string(Index), "test", "scope", Index });
	PF_CHECK(Tests, SegmentService.Begin(SegmentStart, MakeMetadata("segment-0")).has_value());
	for (uint64_t Index = 1; Index <= 80; ++Index)
	{
		EditorPerformanceSnapshot Segment = MakeSnapshot(900, 30'000 + Index);
		Segment.Resources.push_back({ "metric", "test", "scope", Index });
		PF_CHECK(Tests, SegmentService.Observe(Segment, MakeMetadata("segment-" + std::to_string(Index))).has_value());
	}
	EditorPerformanceSnapshot SegmentStop = MakeSnapshot(900, 30'100, false);
	SegmentStop.Resources.push_back({ "metric", "test", "scope", 80 });
	PF_CHECK(Tests, SegmentService.Stop(SegmentStop, MakeMetadata("segment-80")).has_value());
	const auto Segments = SegmentService.GetExportSnapshot();
	PF_CHECK(Tests, Segments && Segments->ResourceSegments.size() == 64);
	PF_CHECK(Tests, Segments && Segments->Snapshot.ResourceSegmentHistoryEvictionCount == 17);
	PF_CHECK(Tests, Segments && Segments->Snapshot.ResourceSegmentMetricTruncationCount == 8);

	EditorPerformanceCaptureService StopBoundaryService;
	EditorPerformanceSnapshot StopBoundaryStart = MakeSnapshot(950, 40'000);
	StopBoundaryStart.Resources.push_back({ "loaded assets", "catalog", "count", 2 });
	PF_CHECK(Tests, StopBoundaryService.Begin(StopBoundaryStart, MakeMetadata("boundary-start")).has_value());
	EditorPerformanceSnapshot StopBoundaryStop = MakeSnapshot(950, 40'200, false);
	StopBoundaryStop.Diagnostics.CaptureEndTimestampNanoseconds = 40'100;
	StopBoundaryStop.Resources.push_back({ "loaded assets", "catalog", "count", 3 });
	PF_CHECK(Tests, StopBoundaryService.Stop(StopBoundaryStop, MakeMetadata("boundary-stop")).has_value());
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
	PF_CHECK(Tests, MetricService.Begin(ManyMetrics, MakeMetadata("many-metrics")).has_value());
	EditorPerformanceSnapshot ManyMetricsStop = MakeSnapshot(950, 35'010, false);
	ManyMetricsStop.Resources = ManyMetrics.Resources;
	PF_CHECK(Tests, MetricService.Stop(ManyMetricsStop, MakeMetadata("many-metrics")).has_value());
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
	PF_CHECK(Tests, ResetService.Begin(FirstSession, MakeMetadata("first-session")).has_value());
	EditorPerformanceSnapshot FirstStop = MakeSnapshot(1000, 40'010, false);
	PF_CHECK(Tests, ResetService.Stop(FirstStop, MakeMetadata("first-session")).has_value());
	EditorPerformanceSnapshot SecondSession = MakeSnapshot(1001, 50'000);
	PF_CHECK(Tests, ResetService.Begin(SecondSession, MakeMetadata("second-session")).has_value());
	EditorPerformanceSnapshot SecondObserve = MakeSnapshot(1001, 50'010);
	SecondObserve.GpuTimingRendererOverwriteCount = 30;
	SecondObserve.GpuTimingInvalidResultCount = 7;
	SecondObserve.CompletedGpuTimings.push_back({ 1000, "retired session", "renderer", "query", 9, 50'001, 400 });
	SecondObserve.CompletedGpuTimings.push_back({ 1001, "current session", "renderer", "query", 10, 50'002, 500 });
	PF_CHECK(Tests, ResetService.Observe(SecondObserve, MakeMetadata("second-session")).has_value());
	EditorPerformanceSnapshot SecondStop = MakeSnapshot(1001, 50'020, false);
	SecondStop.GpuTimingRendererOverwriteCount = 30;
	SecondStop.GpuTimingInvalidResultCount = 7;
	PF_CHECK(Tests, ResetService.Stop(SecondStop, MakeMetadata("second-session")).has_value());
	const auto ResetCapture = ResetService.GetExportSnapshot();
	PF_CHECK(Tests, ResetCapture && ResetCapture->Snapshot.Diagnostics.SessionId == 1001);
	PF_CHECK(Tests, ResetCapture && ResetCapture->Snapshot.GpuTimingRendererOverwriteCount == 30);
	PF_CHECK(Tests, ResetCapture && ResetCapture->Snapshot.GpuTimingInvalidResultCount == 7);
	PF_CHECK(Tests, ResetCapture && ResetCapture->Snapshot.GpuTimingStaleSessionDiscardCount == 1);
	PF_CHECK(Tests, ResetCapture && ResetCapture->Snapshot.GpuTimingCaptureHistoryEvictionCount == 0);
	PF_CHECK(Tests, ResetCapture && ResetCapture->Snapshot.CompletedGpuTimings.size() == 1);
	PF_CHECK(Tests, ResetCapture && ResetCapture->Snapshot.CompletedGpuTimings.front().SessionId == 1001);

	EditorPerformanceCaptureService ShortHistoryService;
	EditorPerformanceSnapshot ShortStart = MakeSnapshot(1100, 60'000);
	PF_CHECK(Tests, ShortHistoryService.Begin(ShortStart, MakeMetadata("short-history")).has_value());
	EditorPerformanceSnapshot ShortObserve = MakeSnapshot(1100, 60'010);
	for (uint64_t Index = 0; Index < 63; ++Index)
		ShortObserve.CompletedGpuTimings.push_back({ 1100, "frame", "renderer", "query", Index, 60'001 + Index, 1 });
	PF_CHECK(Tests, ShortHistoryService.Observe(ShortObserve, MakeMetadata("short-history")).has_value());
	EditorPerformanceSnapshot ShortStop = MakeSnapshot(1100, 60'020, false);
	PF_CHECK(Tests, ShortHistoryService.Stop(ShortStop, MakeMetadata("short-history")).has_value());
	const auto ShortCapture = ShortHistoryService.GetExportSnapshot();
	PF_CHECK(Tests, ShortCapture && ShortCapture->Snapshot.CompletedGpuTimings.size() == 63);
	PF_CHECK(Tests, ShortCapture && ShortCapture->Snapshot.GpuTimingCaptureHistoryEvictionCount == 0);

	EditorPerformanceCaptureService ExactHistoryService;
	EditorPerformanceSnapshot ExactStart = MakeSnapshot(1200, 70'000);
	PF_CHECK(Tests, ExactHistoryService.Begin(ExactStart, MakeMetadata("exact-history")).has_value());
	EditorPerformanceSnapshot ExactObserve = MakeSnapshot(1200, 70'010);
	ExactObserve.GpuTimingRendererOverwriteCount = 5;
	for (uint64_t Index = 0; Index < 64; ++Index)
		ExactObserve.CompletedGpuTimings.push_back({ 1200, "frame", "renderer", "query", Index, 70'001 + Index, 1 });
	PF_CHECK(Tests, ExactHistoryService.Observe(ExactObserve, MakeMetadata("exact-history")).has_value());
	EditorPerformanceSnapshot ExactStop = MakeSnapshot(1200, 70'020, false);
	ExactStop.GpuTimingRendererOverwriteCount = 5;
	PF_CHECK(Tests, ExactHistoryService.Stop(ExactStop, MakeMetadata("exact-history")).has_value());
	const auto ExactCapture = ExactHistoryService.GetExportSnapshot();
	PF_CHECK(Tests, ExactCapture && ExactCapture->Snapshot.CompletedGpuTimings.size() == 64);
	PF_CHECK(Tests, ExactCapture && ExactCapture->Snapshot.GpuTimingCaptureHistoryEvictionCount == 0);
	PF_CHECK(Tests, ExactCapture && ExactCapture->Snapshot.GpuTimingRendererOverwriteCount == 5);

	EditorPerformanceCaptureService CombinedLossService;
	EditorPerformanceSnapshot CombinedStart = MakeSnapshot(1250, 75'000);
	PF_CHECK(Tests, CombinedLossService.Begin(CombinedStart, MakeMetadata("combined-loss")).has_value());
	EditorPerformanceSnapshot CombinedObserve = MakeSnapshot(1250, 75'010);
	CombinedObserve.GpuTimingRendererOverwriteCount = 5;
	for (uint64_t Index = 0; Index < 80; ++Index)
		CombinedObserve.CompletedGpuTimings.push_back({ 1250, "frame", "renderer", "query", Index, 75'001 + Index, 1 });
	PF_CHECK(Tests, CombinedLossService.Observe(CombinedObserve, MakeMetadata("combined-loss")).has_value());
	EditorPerformanceSnapshot CombinedStop = MakeSnapshot(1250, 75'020, false);
	CombinedStop.GpuTimingRendererOverwriteCount = 5;
	PF_CHECK(Tests, CombinedLossService.Stop(CombinedStop, MakeMetadata("combined-loss")).has_value());
	const auto CombinedCapture = CombinedLossService.GetExportSnapshot();
	PF_CHECK(Tests, CombinedCapture && CombinedCapture->Snapshot.GpuTimingCaptureHistoryEvictionCount == 16);
	PF_CHECK(Tests, CombinedCapture && CombinedCapture->Snapshot.GpuTimingRendererOverwriteCount == 5);
	PF_CHECK(Tests, CombinedCapture && CombinedCapture->Snapshot.CompletedGpuTimings.size() == 64);

	EditorPerformanceCaptureService InitialStaleService;
	EditorPerformanceSnapshot InitialStale = MakeSnapshot(1300, 80'000);
	InitialStale.CompletedGpuTimings.push_back({ 1299, "retired result", "renderer", "query", 5, 79'999, 10 });
	PF_CHECK(Tests, InitialStaleService.Begin(InitialStale, MakeMetadata("initial-stale")).has_value());
	EditorPerformanceSnapshot InitialStaleStop = MakeSnapshot(1300, 80'010, false);
	PF_CHECK(Tests, InitialStaleService.Stop(InitialStaleStop, MakeMetadata("initial-stale")).has_value());
	const auto InitialStaleCapture = InitialStaleService.GetExportSnapshot();
	PF_CHECK(Tests, InitialStaleCapture && InitialStaleCapture->Snapshot.GpuTimingStaleSessionDiscardCount == 1);
	PF_CHECK(Tests, InitialStaleCapture && InitialStaleCapture->Snapshot.CompletedGpuTimings.empty());
}
