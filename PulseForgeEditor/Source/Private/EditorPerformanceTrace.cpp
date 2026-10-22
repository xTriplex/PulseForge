#include "Editor/EditorPerformancePanel.h"

#include <nlohmann/json.hpp>

namespace PulseForgeEditor
{
	std::string SerializeEditorPerformanceTrace(
		const EditorPerformanceSnapshot& Snapshot, const EditorPerformanceTraceMetadata& TraceMetadata)
	{
		using Json = nlohmann::ordered_json;
		Json Document;
		Document["displayTimeUnit"] = "ms";
		Document["metadata"] = {
			{ "format", "PulseForge.ChromeTrace" },
			{ "version", 1 },
			{ "platform", TraceMetadata.Platform },
			{ "configuration", TraceMetadata.Configuration },
			{ "renderer", TraceMetadata.Renderer },
			{ "clock", "process-local steady_clock nanoseconds; trace event timestamps are relative microseconds" },
			{ "capture_start_monotonic_ns", Snapshot.Diagnostics.CaptureStartTimestampNanoseconds },
			{ "snapshot_monotonic_ns", Snapshot.Diagnostics.SnapshotTimestampNanoseconds },
			{ "capture_enabled_at_export", Snapshot.Diagnostics.CaptureEnabled },
			{ "event_capacity", Snapshot.Diagnostics.EventCapacity },
			{ "ring_evicted_events", Snapshot.Diagnostics.EvictedCount },
			{ "record_dropped_events", Snapshot.Diagnostics.DroppedCount },
			{ "gpu_timing_available", Snapshot.GpuTimingAvailable },
			{ "gpu_timing_source", Snapshot.GpuTimingAvailable
				? "NVRHI Vulkan timer query; completed main presentation frame duration; delayed collection timestamp is not a synchronized GPU clock"
				: "Unavailable on this renderer" }
		};
		Json& Metadata = Document["metadata"];
		Metadata["resources"] = Json::array();
		for (const EditorResourceMetric& Metric : Snapshot.Resources)
		{
			Json Entry;
			Entry["label"] = Metric.Label;
			Entry["owner"] = Metric.Owner;
			Entry["scope"] = Metric.Scope;
			Entry["value"] = Metric.Value;
			Entry["unit"] = Metric.Kind == EditorResourceMetricKind::Bytes ? "bytes" : "count";
			switch (Metric.Quality)
			{
			case EditorResourceMetricQuality::ExactPayload: Entry["quality"] = "exact-payload"; break;
			case EditorResourceMetricQuality::LogicalAllocation: Entry["quality"] = "logical-allocation-estimate"; break;
			case EditorResourceMetricQuality::ProcessReported: Entry["quality"] = "os-reported"; break;
			}
			Metadata["resources"].push_back(std::move(Entry));
		}
		Metadata["gpu_timings"] = Json::array();
		for (const EditorGpuTimingSample& Sample : Snapshot.CompletedGpuTimings)
		{
			Metadata["gpu_timings"].push_back({
				{ "frame_sequence", Sample.FrameSequence },
				{ "duration_ns", Sample.DurationNanoseconds },
				{ "collected_at_monotonic_ns", Sample.TimestampNanoseconds },
				{ "name", Sample.Name },
				{ "owner", Sample.Owner },
				{ "source", Sample.Source }
			});
		}
		Document["traceEvents"] = Json::array();
		for (const PulseForge::DiagnosticEvent& Event : Snapshot.Diagnostics.Events)
		{
			Json Entry;
			const uint64_t RelativeTimestamp = Event.TimestampNanoseconds >= Snapshot.Diagnostics.CaptureStartTimestampNanoseconds
				? Event.TimestampNanoseconds - Snapshot.Diagnostics.CaptureStartTimestampNanoseconds : 0;
			Entry["ts"] = static_cast<double>(RelativeTimestamp) / 1000.0;
			Entry["pid"] = 1;
			Entry["tid"] = Event.ThreadToken;
			Entry["cat"] = Event.Owner;
			Entry["name"] = Event.Name;
			Entry["args"] = Json::object();
			switch (Event.Kind)
			{
			case PulseForge::DiagnosticEventKind::Duration:
				Entry["ph"] = "X";
				Entry["dur"] = static_cast<double>(Event.DurationNanoseconds) / 1000.0;
				Entry["args"]["owner"] = Event.Owner;
				break;
			case PulseForge::DiagnosticEventKind::Resource:
				Entry["ph"] = "i";
				Entry["s"] = "t";
				Entry["args"]["value"] = Event.Value;
				Entry["args"]["unit"] = Event.Unit;
				break;
			default:
				Entry["ph"] = "C";
				Entry["args"][Event.Unit.empty() ? "value" : Event.Unit] = Event.Value;
				Entry["args"]["owner"] = Event.Owner;
				break;
			}
			Entry["args"]["sequence"] = Event.Sequence;
			Document["traceEvents"].push_back(std::move(Entry));
		}
		return Document.dump(2) + '\n';
	}
} // namespace PulseForgeEditor
