#include "Editor/EditorPerformanceCaptureService.h"

#include "Editor/EditorPlatformFileSystem.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <system_error>

namespace PulseForgeEditor
{
	namespace
	{
		EditorPerformanceExportError MakeError(
			EditorPerformanceExportErrorCode Code, const std::filesystem::path& Path, std::string Message)
		{
			return { Code, Path, std::move(Message) };
		}

		constexpr size_t MaximumTraceEvents = PulseForge::DiagnosticRecorder::MaximumCapacity;
		constexpr size_t MaximumTraceResources = 32;
		constexpr size_t MaximumTraceGpuSamples = 64;

		void RemoveTemporaryFile(const std::filesystem::path& Path) noexcept
		{
			std::error_code Error;
			std::filesystem::remove(Path, Error);
		}
	} // namespace

	bool EditorPerformanceCaptureService::IsGpuTimingAvailable(
		bool Supported, bool EnabledForCapture, bool CaptureActive, bool BackendEnabled, bool FailureReported) noexcept
	{
		return Supported && EnabledForCapture && !FailureReported && (!CaptureActive || BackendEnabled);
	}

	EditorPerformanceCapture EditorPerformanceCaptureService::Capture(
		const EditorPerformanceSnapshot& Snapshot, EditorPerformanceCaptureMetadata Metadata)
	{
		return { Snapshot, std::move(Metadata) };
	}

	std::expected<std::string, EditorPerformanceExportError> EditorPerformanceCaptureService::Serialize(
		const EditorPerformanceCapture& Capture)
	{
		using Json = nlohmann::ordered_json;
		const EditorPerformanceSnapshot& Snapshot = Capture.Snapshot;
		const PulseForge::DiagnosticSnapshot& Diagnostics = Snapshot.Diagnostics;
		try
		{
			Json Document;
			Document["displayTimeUnit"] = "ms";
			Document["metadata"] = {
				{ "format", "PulseForge.ChromeTrace" },
				{ "version", 1 },
				{ "capture_id", Diagnostics.SessionId },
				{ "platform", Capture.Metadata.Platform.empty() ? "unknown" : Capture.Metadata.Platform },
				{ "platform_unavailable_reason", Capture.Metadata.Platform.empty() && Capture.Metadata.PlatformUnavailableReason.empty()
					? "Not supplied by the capture producer" : Capture.Metadata.PlatformUnavailableReason },
				{ "configuration", Capture.Metadata.Configuration.empty() ? "unknown" : Capture.Metadata.Configuration },
				{ "configuration_unavailable_reason", Capture.Metadata.Configuration.empty() && Capture.Metadata.ConfigurationUnavailableReason.empty()
					? "Not supplied by the capture producer" : Capture.Metadata.ConfigurationUnavailableReason },
				{ "renderer", Capture.Metadata.Renderer.empty() ? "unknown" : Capture.Metadata.Renderer },
				{ "renderer_unavailable_reason", Capture.Metadata.Renderer.empty() && Capture.Metadata.RendererUnavailableReason.empty()
					? "Not supplied by the capture producer" : Capture.Metadata.RendererUnavailableReason },
				{ "workload", Capture.Metadata.Workload },
				{ "engine_build_identifier", Capture.Metadata.BuildIdentifier.empty() ? "unavailable" : Capture.Metadata.BuildIdentifier },
				{ "engine_build_identifier_unavailable_reason", Capture.Metadata.BuildIdentifierUnavailableReason.empty() &&
					Capture.Metadata.BuildIdentifier.empty() ? "Not supplied by the capture producer" : Capture.Metadata.BuildIdentifierUnavailableReason },
				{ "hardware", Capture.Metadata.Hardware.empty() ? "unavailable" : Capture.Metadata.Hardware },
				{ "hardware_unavailable_reason", Capture.Metadata.HardwareUnavailableReason.empty() && Capture.Metadata.Hardware.empty()
					? "Not supplied by the capture producer" : Capture.Metadata.HardwareUnavailableReason },
				{ "content_fingerprint", Capture.Metadata.ContentFingerprint },
				{ "content_fingerprint_unavailable_reason", Capture.Metadata.ContentFingerprintUnavailableReason.empty() &&
					Capture.Metadata.ContentFingerprint.empty() ? "Not supplied by the capture producer" : Capture.Metadata.ContentFingerprintUnavailableReason },
				{ "clock", "process-local steady_clock nanoseconds; trace event timestamps are relative microseconds" },
				{ "capture_start_monotonic_ns", Diagnostics.CaptureStartTimestampNanoseconds },
				{ "capture_stop_monotonic_ns", Diagnostics.CaptureEndTimestampNanoseconds },
				{ "snapshot_monotonic_ns", Diagnostics.SnapshotTimestampNanoseconds },
				{ "capture_enabled_at_export", Diagnostics.CaptureEnabled },
				{ "event_capacity", Diagnostics.EventCapacity },
				{ "ring_evicted_events", Diagnostics.EvictedCount },
				{ "record_dropped_events", Diagnostics.DroppedCount },
				{ "malformed_utf8_events", Diagnostics.MalformedUtf8Count },
				{ "truncated_string_events", Diagnostics.TruncatedStringCount },
				{ "retired_duration_scopes_discarded", Diagnostics.RetiredDurationScopesDiscarded },
				{ "diagnostic_events_in_snapshot", Diagnostics.Events.size() },
				{ "diagnostic_events_exported", (std::min)(Diagnostics.Events.size(), MaximumTraceEvents) },
				{ "gpu_timing_available", Snapshot.GpuTimingAvailable },
				{ "gpu_timing_supported", Snapshot.GpuTimingSupported },
				{ "gpu_timing_enabled", Snapshot.GpuTimingEnabled },
				{ "gpu_timing_dropped_or_overwritten", Snapshot.GpuTimingDroppedCount },
				{ "gpu_timing_stale_session_discarded", Snapshot.GpuTimingStaleSessionDiscardCount },
				{ "gpu_timing_unavailable_reason", Snapshot.GpuTimingAvailable ? "" : Snapshot.GpuTimingUnavailableReason },
				{ "gpu_timing_source", Snapshot.GpuTimingAvailable
					? "NVRHI Vulkan timer query; completed main presentation frame duration; delayed collection timestamp is not a synchronized GPU clock"
					: Snapshot.GpuTimingUnavailableReason }
			};
			Json& Metadata = Document["metadata"];
			Metadata["resources"] = Json::array();
			Metadata["resource_metrics_in_snapshot"] = Snapshot.Resources.size();
			Metadata["resource_metrics_exported"] = (std::min)(Snapshot.Resources.size(), MaximumTraceResources);
			for (size_t Index = 0; Index < Metadata["resource_metrics_exported"].get<size_t>(); ++Index)
			{
				const EditorResourceMetric& Metric = Snapshot.Resources[Index];
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
				case EditorResourceMetricQuality::Unavailable: Entry["quality"] = "unavailable"; break;
				}
				if (!Metric.AvailabilityReason.empty()) Entry["unavailable_reason"] = Metric.AvailabilityReason;
				if (!Metric.QualityReason.empty()) Entry["quality_reason"] = Metric.QualityReason;
				Metadata["resources"].push_back(std::move(Entry));
			}
			Metadata["gpu_timings"] = Json::array();
			Metadata["gpu_timing_samples_in_snapshot"] = Snapshot.CompletedGpuTimings.size();
			Metadata["gpu_timing_samples_exported"] = (std::min)(Snapshot.CompletedGpuTimings.size(), MaximumTraceGpuSamples);
			for (size_t Index = 0; Index < Metadata["gpu_timing_samples_exported"].get<size_t>(); ++Index)
			{
				const EditorGpuTimingSample& Sample = Snapshot.CompletedGpuTimings[Index];
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
			const size_t EventCount = (std::min)(Diagnostics.Events.size(), MaximumTraceEvents);
			for (size_t Index = 0; Index < EventCount; ++Index)
			{
				const PulseForge::DiagnosticEvent& Event = Diagnostics.Events[Index];
				const uint64_t EventStart = Event.Kind == PulseForge::DiagnosticEventKind::Duration &&
					Event.StartTimestampNanoseconds != 0 ? Event.StartTimestampNanoseconds : Event.TimestampNanoseconds;
				const uint64_t RelativeTimestamp = EventStart >= Diagnostics.CaptureStartTimestampNanoseconds
					? EventStart - Diagnostics.CaptureStartTimestampNanoseconds : 0;
				Json Entry;
				Entry["ts"] = static_cast<double>(RelativeTimestamp) / 1000.0;
				Entry["pid"] = 1;
				Entry["tid"] = Event.ThreadToken;
				Entry["cat"] = Event.Owner;
				Entry["name"] = Event.Name;
				Entry["args"] = Json::object();
				Entry["args"]["session_id"] = Event.SessionId;
				switch (Event.Kind)
				{
				case PulseForge::DiagnosticEventKind::Duration:
					Entry["ph"] = "X";
					Entry["dur"] = static_cast<double>(Event.DurationNanoseconds) / 1000.0;
					Entry["args"]["owner"] = Event.Owner;
					Entry["args"]["completion_timestamp_ns"] = Event.TimestampNanoseconds;
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
		catch (const std::exception& Error)
		{
			return std::unexpected(MakeError(EditorPerformanceExportErrorCode::SerializeFailed, {}, Error.what()));
		}
	}

	std::expected<void, EditorPerformanceExportError> EditorPerformanceCaptureService::ExportAtomically(
		const std::filesystem::path& Destination, const EditorPerformanceCapture& Capture)
	{
		if (Destination.empty())
			return std::unexpected(MakeError(EditorPerformanceExportErrorCode::InvalidCapture, Destination,
				"The capture destination path is empty"));

		auto Contents = Serialize(Capture);
		if (!Contents) return std::unexpected(std::move(Contents.error()));

		static std::atomic<uint64_t> TemporarySequence = 0;
		const uint64_t Timestamp = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
		std::filesystem::path TemporaryPath;
		try
		{
			TemporaryPath = Destination;
			TemporaryPath += ".tmp." + std::to_string(Capture.Snapshot.Diagnostics.SessionId) + "." +
				std::to_string(Timestamp) + "." + std::to_string(TemporarySequence.fetch_add(1, std::memory_order_relaxed));
			{
				std::ofstream Output(TemporaryPath, std::ios::binary | std::ios::trunc);
				if (!Output)
				{
					RemoveTemporaryFile(TemporaryPath);
					return std::unexpected(MakeError(EditorPerformanceExportErrorCode::TemporaryWriteFailed, TemporaryPath,
						"Could not create the temporary performance capture file"));
				}
				Output.write(Contents->data(), static_cast<std::streamsize>(Contents->size()));
				Output.flush();
				Output.close();
				if (!Output)
				{
					RemoveTemporaryFile(TemporaryPath);
					return std::unexpected(MakeError(EditorPerformanceExportErrorCode::TemporaryWriteFailed, TemporaryPath,
						"Could not write the temporary performance capture file"));
				}
			}
		}
		catch (const std::exception& Error)
		{
			if (!TemporaryPath.empty()) RemoveTemporaryFile(TemporaryPath);
			return std::unexpected(MakeError(EditorPerformanceExportErrorCode::TemporaryWriteFailed,
				TemporaryPath.empty() ? Destination : TemporaryPath, Error.what()));
		}

		if (auto Replaced = EditorPlatformFileSystem::ReplaceFileAtomically(TemporaryPath, Destination); !Replaced)
		{
			RemoveTemporaryFile(TemporaryPath);
			return std::unexpected(MakeError(EditorPerformanceExportErrorCode::AtomicReplaceFailed, Destination,
				std::move(Replaced.error())));
		}
		return {};
	}
} // namespace PulseForgeEditor
