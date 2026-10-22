#include "TestSupport.h"

#include "Editor/EditorPerformancePanel.h"

#include <nlohmann/json.hpp>

void RunEditorPerformanceTraceTests(TestRunner& Tests)
{
	using namespace PulseForgeEditor;
	EditorPerformanceSnapshot Snapshot;
	Snapshot.Diagnostics.CaptureStartTimestampNanoseconds = 1'000'000;
	Snapshot.Diagnostics.SnapshotTimestampNanoseconds = 8'000'000;
	Snapshot.Diagnostics.EventCapacity = 8192;
	Snapshot.Diagnostics.EvictedCount = 3;
	Snapshot.Diagnostics.DroppedCount = 2;
	Snapshot.Diagnostics.Events.push_back({
		.Sequence = 9,
		.TimestampNanoseconds = 3'500'000,
		.ThreadToken = 17,
		.Kind = PulseForge::DiagnosticEventKind::Duration,
		.Name = "scene update",
		.Owner = "SceneRuntime",
		.Unit = "ns",
		.DurationNanoseconds = 2'000'000
	});
	Snapshot.Diagnostics.Events.push_back({
		.Sequence = 10,
		.TimestampNanoseconds = 4'000'000,
		.ThreadToken = 17,
		.Kind = PulseForge::DiagnosticEventKind::Counter,
		.Name = "cache hits",
		.Owner = "PreviewCache",
		.Unit = "hits",
		.Value = 12
	});
	Snapshot.Resources.push_back({ "Preview bytes", "PreviewCache", "Logical texture footprint", 4096,
		EditorResourceMetricKind::Bytes, EditorResourceMetricQuality::LogicalAllocation });
	Snapshot.GpuTimingAvailable = true;
	Snapshot.CompletedGpuTimings.push_back({ "main frame", "VulkanRenderer", "delayed query", 22, 5'000'000, 1'250'000 });

	const EditorPerformanceTraceMetadata Metadata{ "Windows", "Debug", "Vulkan" };
	const nlohmann::json Trace = nlohmann::json::parse(SerializeEditorPerformanceTrace(Snapshot, Metadata));
	PF_CHECK(Tests, Trace["metadata"]["format"] == "PulseForge.ChromeTrace");
	PF_CHECK(Tests, Trace["metadata"]["platform"] == "Windows");
	PF_CHECK(Tests, Trace["metadata"]["configuration"] == "Debug");
	PF_CHECK(Tests, Trace["metadata"]["renderer"] == "Vulkan");
	PF_CHECK(Tests, Trace["metadata"]["capture_start_monotonic_ns"] == 1'000'000);
	PF_CHECK(Tests, Trace["metadata"]["event_capacity"] == 8192);
	PF_CHECK(Tests, Trace["metadata"]["ring_evicted_events"] == 3);
	PF_CHECK(Tests, Trace["metadata"]["record_dropped_events"] == 2);
	PF_CHECK(Tests, Trace["metadata"]["resources"][0]["quality"] == "logical-allocation-estimate");
	PF_CHECK(Tests, Trace["metadata"].contains("gpu_timings"));
	PF_CHECK(Tests, Trace["traceEvents"].size() == 2);
	if (Trace["traceEvents"].size() != 2) return;
	PF_CHECK(Tests, Trace["traceEvents"][0]["ph"] == "X");
	PF_CHECK(Tests, Trace["traceEvents"][0]["ts"] == 2500.0);
	PF_CHECK(Tests, Trace["traceEvents"][0]["dur"] == 2000.0);
	PF_CHECK(Tests, Trace["traceEvents"][0]["tid"] == 17);
	PF_CHECK(Tests, Trace["traceEvents"][1]["ph"] == "C");
	PF_CHECK(Tests, Trace["traceEvents"][1]["args"]["hits"] == 12);
}
