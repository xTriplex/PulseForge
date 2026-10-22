#include "TestSupport.h"

#include "Editor/EditorPerformanceCaptureService.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>

void RunEditorPerformanceTraceTests(TestRunner& Tests)
{
	using namespace PulseForgeEditor;
	EditorPerformanceSnapshot Snapshot;
	PulseForge::DiagnosticRecorder Recorder(8);
	Recorder.SetCaptureEnabled(true);
	{
		PulseForge::ScopedDiagnosticTimer RetiredTimer(Recorder, "retired timer", "SceneRuntime");
		Recorder.SetCaptureEnabled(false);
		Recorder.SetCaptureEnabled(true);
	}
	PF_CHECK(Tests, Recorder.CaptureSnapshot().RetiredDurationScopesDiscarded == 1);
	PF_CHECK(Tests, Recorder.RecordEvent(PulseForge::DiagnosticEventKind::Counter, "cache hits", "PreviewCache", "hits", 12));
	{
		PulseForge::ScopedDiagnosticTimer Timer(Recorder, "scene update", "SceneRuntime");
	}
	Recorder.SetCaptureEnabled(false);
	Snapshot.Diagnostics = Recorder.CaptureSnapshot();
	Snapshot.Resources.push_back({ "Preview bytes", "PreviewCache", "Logical texture footprint", 4096,
		EditorResourceMetricKind::Bytes, EditorResourceMetricQuality::LogicalAllocation });
	Snapshot.Resources.push_back({ "Native image bytes", "Renderer", "Not provided by backend", 0,
		EditorResourceMetricKind::Bytes, EditorResourceMetricQuality::Unavailable, "backend reports no native allocation size" });
	Snapshot.GpuTimingAvailable = true;
	Snapshot.CompletedGpuTimings.push_back({ "main frame", "VulkanRenderer", "delayed query", 22, 5'000'000, 1'250'000 });

	const EditorPerformanceCapture Capture = EditorPerformanceCaptureService::Capture(Snapshot,
		EditorPerformanceCaptureMetadata{ .Platform = "Windows", .Configuration = "Debug", .Renderer = "Vulkan",
			.Workload = "scene-runtime-regression" });
	const uint64_t CaptureSessionId = Snapshot.Diagnostics.SessionId;
	const uint64_t CounterEventTime = Snapshot.Diagnostics.Events[0].TimestampNanoseconds;
	const uint64_t DurationStartTime = Snapshot.Diagnostics.Events[1].StartTimestampNanoseconds;
	const uint64_t DurationEndTime = Snapshot.Diagnostics.Events[1].TimestampNanoseconds;
	Snapshot.Diagnostics.Events.clear();
	const auto SerializedTrace = EditorPerformanceCaptureService::Serialize(Capture);
	PF_CHECK(Tests, SerializedTrace.has_value());
	if (!SerializedTrace) return;
	const nlohmann::json Trace = nlohmann::json::parse(*SerializedTrace);
	PF_CHECK(Tests, Trace["metadata"]["format"] == "PulseForge.ChromeTrace");
	PF_CHECK(Tests, Trace["metadata"]["platform"] == "Windows");
	PF_CHECK(Tests, Trace["metadata"]["configuration"] == "Debug");
	PF_CHECK(Tests, Trace["metadata"]["renderer"] == "Vulkan");
	PF_CHECK(Tests, Trace["metadata"]["workload"] == "scene-runtime-regression");
	PF_CHECK(Tests, Trace["metadata"]["capture_id"] == CaptureSessionId);
	PF_CHECK(Tests, Trace["metadata"]["capture_start_monotonic_ns"] == Snapshot.Diagnostics.CaptureStartTimestampNanoseconds);
	PF_CHECK(Tests, Trace["metadata"]["capture_stop_monotonic_ns"] == Snapshot.Diagnostics.CaptureEndTimestampNanoseconds);
	PF_CHECK(Tests, Trace["metadata"]["event_capacity"] == 8);
	PF_CHECK(Tests, Trace["metadata"]["ring_evicted_events"] == 0);
	PF_CHECK(Tests, Trace["metadata"]["record_dropped_events"] == 0);
	PF_CHECK(Tests, Trace["metadata"]["malformed_utf8_events"] == 0);
	PF_CHECK(Tests, Trace["metadata"]["truncated_string_events"] == 0);
	PF_CHECK(Tests, Trace["metadata"]["retired_duration_scopes_discarded"] == 1);
	PF_CHECK(Tests, Trace["metadata"]["resources"][0]["quality"] == "logical-allocation-estimate");
	PF_CHECK(Tests, Trace["metadata"]["resources"][1]["quality"] == "unavailable");
	PF_CHECK(Tests, Trace["metadata"]["resources"][1]["unavailable_reason"] == "backend reports no native allocation size");
	PF_CHECK(Tests, Trace["metadata"].contains("gpu_timings"));
	PF_CHECK(Tests, Trace["traceEvents"].size() == 2);
	if (Trace["traceEvents"].size() != 2) return;
	PF_CHECK(Tests, Trace["traceEvents"][0]["ph"] == "C");
	PF_CHECK(Tests, Trace["traceEvents"][0]["ts"] ==
		static_cast<double>(CounterEventTime - Capture.Snapshot.Diagnostics.CaptureStartTimestampNanoseconds) / 1000.0);
	PF_CHECK(Tests, Trace["traceEvents"][0]["args"]["hits"] == 12);
	PF_CHECK(Tests, Trace["traceEvents"][0]["args"]["session_id"] == CaptureSessionId);
	PF_CHECK(Tests, Trace["traceEvents"][1]["ph"] == "X");
	PF_CHECK(Tests, Trace["traceEvents"][1]["ts"] ==
		static_cast<double>(DurationStartTime - Capture.Snapshot.Diagnostics.CaptureStartTimestampNanoseconds) / 1000.0);
	PF_CHECK(Tests, Trace["traceEvents"][1]["dur"] == static_cast<double>(DurationEndTime - DurationStartTime) / 1000.0);
	PF_CHECK(Tests, Trace["traceEvents"][1]["args"]["session_id"] == CaptureSessionId);
	PF_CHECK(Tests, Trace["traceEvents"][1]["args"]["completion_timestamp_ns"] == DurationEndTime);
	const auto EmptyPathExport = EditorPerformanceCaptureService::ExportAtomically({}, Capture);
	PF_CHECK(Tests, !EmptyPathExport);
	PF_CHECK(Tests, !EmptyPathExport && EmptyPathExport.error().Code == EditorPerformanceExportErrorCode::InvalidCapture);

	const std::filesystem::path ExportFailureRoot = std::filesystem::temp_directory_path() /
		("PulseForgeTraceExportFailure-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	std::error_code FileError;
	const bool CreatedExportFailureRoot = std::filesystem::create_directory(ExportFailureRoot, FileError);
	PF_CHECK(Tests, CreatedExportFailureRoot && !FileError);
	if (CreatedExportFailureRoot)
	{
		const std::filesystem::path ExistingDestination = ExportFailureRoot / "existing-capture.json";
		FileError.clear();
		const bool CreatedExistingDestination = std::filesystem::create_directory(ExistingDestination, FileError);
		PF_CHECK(Tests, CreatedExistingDestination && !FileError);
		if (CreatedExistingDestination)
		{
			const std::filesystem::path Sentinel = ExistingDestination / "preserve-me.txt";
			std::ofstream ExistingContent(Sentinel, std::ios::binary | std::ios::trunc);
			ExistingContent << "existing destination must survive export failure";
			ExistingContent.close();
			const auto FailedReplace = EditorPerformanceCaptureService::ExportAtomically(ExistingDestination, Capture);
			PF_CHECK(Tests, !FailedReplace);
			PF_CHECK(Tests, !FailedReplace && FailedReplace.error().Code == EditorPerformanceExportErrorCode::AtomicReplaceFailed);
			PF_CHECK(Tests, std::filesystem::is_directory(ExistingDestination));
			std::ifstream PreservedContent(Sentinel, std::ios::binary);
			const std::string PreservedBytes((std::istreambuf_iterator<char>(PreservedContent)),
				std::istreambuf_iterator<char>());
			PF_CHECK(Tests, PreservedBytes == "existing destination must survive export failure");
			const auto RemainingEntries = std::distance(
				std::filesystem::directory_iterator(ExportFailureRoot), std::filesystem::directory_iterator{});
			PF_CHECK(Tests, RemainingEntries == 1);
			const std::filesystem::path SuccessfulDestination = ExportFailureRoot / "successful-capture.json";
			const auto ReusedCaptureExport = EditorPerformanceCaptureService::ExportAtomically(SuccessfulDestination, Capture);
			PF_CHECK(Tests, ReusedCaptureExport.has_value());
			PF_CHECK(Tests, std::filesystem::is_regular_file(SuccessfulDestination));
		}
		FileError.clear();
		std::filesystem::remove_all(ExportFailureRoot, FileError);
		PF_CHECK(Tests, !FileError);
	}

	Snapshot.Diagnostics.CaptureEnabled = true;
	Snapshot.Diagnostics.CaptureEndTimestampNanoseconds = 0;
	const auto ActiveTrace = EditorPerformanceCaptureService::Serialize(
		EditorPerformanceCaptureService::Capture(Snapshot, { "Windows", "Debug", "Vulkan", "active-capture" }));
	PF_CHECK(Tests, ActiveTrace.has_value());
	if (ActiveTrace)
	{
		const nlohmann::json ActiveDocument = nlohmann::json::parse(*ActiveTrace);
		PF_CHECK(Tests, ActiveDocument["metadata"]["capture_enabled_at_export"] == true);
		PF_CHECK(Tests, ActiveDocument["metadata"]["capture_stop_monotonic_ns"] == 0);
	}
}
