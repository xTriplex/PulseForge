#include "Editor/EditorPerformanceCaptureService.h"

#include "Editor/EditorPlatformFileSystem.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <system_error>
#include <utility>

namespace PulseForgeEditor
{
	namespace
	{
		EditorPerformanceCaptureError MakeCaptureError(EditorPerformanceCaptureErrorCode Code, std::string Message)
		{
			return { Code, std::move(Message) };
		}

		EditorPerformanceExportError MakeExportError(
			EditorPerformanceExportErrorCode Code, const std::filesystem::path& Path, std::string Message)
		{
			return { Code, Path, std::move(Message) };
		}

		void RemoveTemporaryFile(const std::filesystem::path& Path) noexcept
		{
			std::error_code Error;
			std::filesystem::remove(Path, Error);
		}

		const char* GpuResultStatusName(EditorGpuTimingResultStatus Status) noexcept
		{
			switch (Status)
			{
			case EditorGpuTimingResultStatus::Pending: return "pending";
			case EditorGpuTimingResultStatus::Informative: return "informative";
			case EditorGpuTimingResultStatus::ZeroDurationOnly: return "zero-duration-only";
			case EditorGpuTimingResultStatus::NoSamples: return "no-samples";
			case EditorGpuTimingResultStatus::Failed: return "failed";
			default: return "unknown";
			}
		}

		bool MetricsEqual(const std::vector<EditorResourceMetric>& Left, const std::vector<EditorResourceMetric>& Right)
		{
			if (Left.size() != Right.size()) return false;
			for (size_t Index = 0; Index < Left.size(); ++Index)
			{
				const EditorResourceMetric& A = Left[Index];
				const EditorResourceMetric& B = Right[Index];
				if (A.Label != B.Label || A.Owner != B.Owner || A.Scope != B.Scope || A.Value != B.Value || A.Kind != B.Kind ||
					A.Quality != B.Quality || A.AvailabilityReason != B.AvailabilityReason || A.QualityReason != B.QualityReason)
					return false;
			}
			return true;
		}

		void KeepLatestMetrics(std::vector<EditorResourceMetric>& Metrics, size_t Maximum)
		{
			if (Metrics.size() > Maximum)
				Metrics.erase(Metrics.begin(), Metrics.end() - static_cast<std::ptrdiff_t>(Maximum));
		}

		void KeepLatestEvents(PulseForge::DiagnosticSnapshot& Snapshot, uint64_t& TruncatedCount)
		{
			const size_t Maximum = PulseForge::DiagnosticRecorder::MaximumCapacity;
			if (Snapshot.Events.size() <= Maximum) return;
			const size_t Truncated = Snapshot.Events.size() - Maximum;
			Snapshot.Events.erase(Snapshot.Events.begin(), Snapshot.Events.begin() + static_cast<std::ptrdiff_t>(Truncated));
			TruncatedCount += Truncated;
		}
	} // namespace

	std::expected<void, EditorPerformanceCaptureError> EditorPerformanceCaptureService::Begin(
		const EditorPerformanceSnapshot& InitialSnapshot, EditorPerformanceCaptureMetadata Metadata)
	{
		if (m_ActiveCapture)
			return std::unexpected(MakeCaptureError(EditorPerformanceCaptureErrorCode::AlreadyActive,
				"A performance capture is already active"));
		if (InitialSnapshot.Diagnostics.SessionId == 0 || InitialSnapshot.Diagnostics.CaptureStartTimestampNanoseconds == 0)
			return std::unexpected(MakeCaptureError(EditorPerformanceCaptureErrorCode::StaleSession,
				"The initial diagnostics snapshot has no active capture identity"));

		m_CompletedCapture.reset();
		m_GpuHistory.clear();
		m_LastRendererOverwriteCount = InitialSnapshot.GpuTimingRendererOverwriteCount;
		m_LastInvalidResultCount = InitialSnapshot.GpuTimingInvalidResultCount;
		m_LastGpuFrameSequence = 0;
		m_HasLastGpuFrameSequence = false;

		m_ActiveCapture.emplace();
		m_ActiveCapture->Metadata = Metadata;
		m_ActiveCapture->Snapshot = InitialSnapshot;
		m_ActiveCapture->Snapshot.DiagnosticSnapshotTruncationCount = 0;
		KeepLatestEvents(m_ActiveCapture->Snapshot.Diagnostics, m_ActiveCapture->Snapshot.DiagnosticSnapshotTruncationCount);
		KeepLatestMetrics(m_ActiveCapture->Snapshot.Resources, MaximumMetricsPerResourceSegment);
		m_ActiveCapture->Snapshot.CompletedGpuTimings.clear();
		m_ActiveCapture->Snapshot.GpuTimingRendererOverwriteCount = 0;
		m_ActiveCapture->Snapshot.GpuTimingCaptureHistoryEvictionCount = 0;
		m_ActiveCapture->Snapshot.GpuTimingStaleSessionDiscardCount = 0;
		m_ActiveCapture->Snapshot.GpuTimingInvalidResultCount = 0;
		m_ActiveCapture->Snapshot.GpuTimingPositiveDurationCount = 0;
		m_ActiveCapture->Snapshot.GpuTimingZeroDurationCount = 0;
		m_ActiveCapture->Snapshot.GpuTimingExportTruncationCount = 0;
		m_ActiveCapture->Snapshot.ResourceSegmentHistoryEvictionCount = 0;
		m_ActiveCapture->Snapshot.ResourceSegmentMetricTruncationCount = 0;
		AppendGpuSamples(*m_ActiveCapture, InitialSnapshot);
		AppendResourceSegment(InitialSnapshot, Metadata);
		UpdateGpuStatus(m_ActiveCapture->Snapshot, false);
		return {};
	}

	std::expected<void, EditorPerformanceCaptureError> EditorPerformanceCaptureService::Observe(
		const EditorPerformanceSnapshot& Snapshot, const EditorPerformanceCaptureMetadata& Metadata)
	{
		EditorPerformanceCapture* Target = m_ActiveCapture ? &*m_ActiveCapture : m_CompletedCapture ? &*m_CompletedCapture : nullptr;
		if (!Target)
			return std::unexpected(MakeCaptureError(EditorPerformanceCaptureErrorCode::NotActive,
				"No performance capture is active"));
		AppendGpuSamples(*Target, Snapshot);
		if (Snapshot.Diagnostics.SessionId != Target->Snapshot.Diagnostics.SessionId)
		{
			UpdateGpuStatus(Target->Snapshot, !m_ActiveCapture);
			return std::unexpected(MakeCaptureError(EditorPerformanceCaptureErrorCode::StaleSession,
				"The diagnostics snapshot belongs to a different capture session"));
		}

		const uint64_t CurrentOverwriteCount = Snapshot.GpuTimingRendererOverwriteCount;
		Target->Snapshot.GpuTimingRendererOverwriteCount += CurrentOverwriteCount >= m_LastRendererOverwriteCount
			? CurrentOverwriteCount - m_LastRendererOverwriteCount : CurrentOverwriteCount;
		m_LastRendererOverwriteCount = CurrentOverwriteCount;
		const uint64_t CurrentInvalidCount = Snapshot.GpuTimingInvalidResultCount;
		Target->Snapshot.GpuTimingInvalidResultCount += CurrentInvalidCount >= m_LastInvalidResultCount
			? CurrentInvalidCount - m_LastInvalidResultCount : CurrentInvalidCount;
		m_LastInvalidResultCount = CurrentInvalidCount;
		if (!m_ActiveCapture)
		{
			if (Snapshot.GpuTimingFailureReported)
			{
				Target->Snapshot.GpuTimingFailureReported = true;
				Target->Snapshot.GpuTimingUnavailableReason = Snapshot.GpuTimingUnavailableReason;
			}
			UpdateGpuStatus(Target->Snapshot, true);
			return {};
		}

		AppendResourceSegment(Snapshot, Metadata);
		Target->Snapshot.Diagnostics = Snapshot.Diagnostics;
		KeepLatestEvents(Target->Snapshot.Diagnostics, Target->Snapshot.DiagnosticSnapshotTruncationCount);
		Target->Snapshot.Resources = Snapshot.Resources;
		KeepLatestMetrics(Target->Snapshot.Resources, MaximumMetricsPerResourceSegment);
		Target->Snapshot.GpuTimingSupported = Snapshot.GpuTimingSupported;
		Target->Snapshot.GpuTimingFailureReported = Snapshot.GpuTimingFailureReported;
		Target->Snapshot.GpuTimingUnavailableReason = Snapshot.GpuTimingUnavailableReason;
		Target->Snapshot.GpuTimingRequested = Snapshot.GpuTimingRequested;
		Target->Snapshot.GpuTimingEnabled = Target->Snapshot.GpuTimingEnabled || Snapshot.GpuTimingEnabled;
		UpdateGpuStatus(Target->Snapshot, false);
		return {};
	}

	std::expected<void, EditorPerformanceCaptureError> EditorPerformanceCaptureService::Stop(
		const EditorPerformanceSnapshot& FinalSnapshot, EditorPerformanceCaptureMetadata FinalMetadata)
	{
		if (!m_ActiveCapture)
			return std::unexpected(MakeCaptureError(EditorPerformanceCaptureErrorCode::NotActive,
				"No performance capture is active"));
		EditorPerformanceSnapshot BoundedFinalSnapshot = FinalSnapshot;
		const uint64_t CaptureEnd = BoundedFinalSnapshot.Diagnostics.CaptureEndTimestampNanoseconds;
		if (CaptureEnd != 0 && BoundedFinalSnapshot.Diagnostics.SnapshotTimestampNanoseconds > CaptureEnd)
			BoundedFinalSnapshot.Diagnostics.SnapshotTimestampNanoseconds = CaptureEnd;
		if (auto Observed = Observe(BoundedFinalSnapshot, FinalMetadata); !Observed) return Observed;
		UpdateGpuStatus(m_ActiveCapture->Snapshot, true);
		m_ActiveCapture->Snapshot.Diagnostics.CaptureEnabled = false;
		m_CompletedCapture = std::move(m_ActiveCapture);
		m_ActiveCapture.reset();
		return {};
	}

	std::optional<EditorPerformanceCapture> EditorPerformanceCaptureService::GetExportSnapshot() const
	{
		if (m_ActiveCapture)
		{
			EditorPerformanceCapture Snapshot = *m_ActiveCapture;
			// An active point-in-time export is bounded by SnapshotTimestampNanoseconds,
			// but it must not claim the capture itself has stopped.
			Snapshot.Snapshot.Diagnostics.CaptureEndTimestampNanoseconds = 0;
			return Snapshot;
		}
		return m_CompletedCapture;
	}

	std::optional<EditorPerformanceSnapshot> EditorPerformanceCaptureService::GetCurrentSnapshot() const
	{
		if (m_ActiveCapture) return m_ActiveCapture->Snapshot;
		if (m_CompletedCapture) return m_CompletedCapture->Snapshot;
		return std::nullopt;
	}

	uint64_t EditorPerformanceCaptureService::GetSessionId() const noexcept
	{
		if (m_ActiveCapture) return m_ActiveCapture->Snapshot.Diagnostics.SessionId;
		return m_CompletedCapture ? m_CompletedCapture->Snapshot.Diagnostics.SessionId : 0;
	}

	void EditorPerformanceCaptureService::ApplyGpuHistory(EditorPerformanceSnapshot& Snapshot) const
	{
		const EditorPerformanceSnapshot* CaptureSnapshot = m_ActiveCapture ? &m_ActiveCapture->Snapshot :
			m_CompletedCapture ? &m_CompletedCapture->Snapshot : nullptr;
		if (!CaptureSnapshot || Snapshot.Diagnostics.SessionId != CaptureSnapshot->Diagnostics.SessionId) return;
		Snapshot.GpuTimingRequested = CaptureSnapshot->GpuTimingRequested;
		Snapshot.GpuTimingEnabled = CaptureSnapshot->GpuTimingEnabled;
		Snapshot.GpuTimingAvailable = CaptureSnapshot->GpuTimingAvailable;
		Snapshot.GpuTimingResultStatus = CaptureSnapshot->GpuTimingResultStatus;
		Snapshot.GpuTimingRendererOverwriteCount = CaptureSnapshot->GpuTimingRendererOverwriteCount;
		Snapshot.GpuTimingCaptureHistoryEvictionCount = CaptureSnapshot->GpuTimingCaptureHistoryEvictionCount;
		Snapshot.GpuTimingStaleSessionDiscardCount = CaptureSnapshot->GpuTimingStaleSessionDiscardCount;
		Snapshot.GpuTimingInvalidResultCount = CaptureSnapshot->GpuTimingInvalidResultCount;
		Snapshot.GpuTimingPositiveDurationCount = CaptureSnapshot->GpuTimingPositiveDurationCount;
		Snapshot.GpuTimingZeroDurationCount = CaptureSnapshot->GpuTimingZeroDurationCount;
		Snapshot.GpuTimingExportTruncationCount = CaptureSnapshot->GpuTimingExportTruncationCount;
		Snapshot.CompletedGpuTimings = CaptureSnapshot->CompletedGpuTimings;
		if (!CaptureSnapshot->GpuTimingUnavailableReason.empty())
			Snapshot.GpuTimingUnavailableReason = CaptureSnapshot->GpuTimingUnavailableReason;
	}

	void EditorPerformanceCaptureService::AppendGpuSamples(
		EditorPerformanceCapture& Target, const EditorPerformanceSnapshot& Snapshot)
	{
		const uint64_t SessionId = Target.Snapshot.Diagnostics.SessionId;
		for (const EditorGpuTimingSample& Incoming : Snapshot.CompletedGpuTimings)
		{
			if (Incoming.SessionId != SessionId)
			{
				++Target.Snapshot.GpuTimingStaleSessionDiscardCount;
				continue;
			}
			if (m_HasLastGpuFrameSequence && Incoming.FrameSequence <= m_LastGpuFrameSequence) continue;
			m_LastGpuFrameSequence = Incoming.FrameSequence;
			m_HasLastGpuFrameSequence = true;
			EditorGpuTimingSample Sample = Incoming;
			Sample.Quality = Sample.DurationNanoseconds == 0
				? EditorGpuTimingSampleQuality::ZeroDuration : EditorGpuTimingSampleQuality::PositiveDuration;
			if (Sample.Quality == EditorGpuTimingSampleQuality::ZeroDuration)
				++Target.Snapshot.GpuTimingZeroDurationCount;
			else
				++Target.Snapshot.GpuTimingPositiveDurationCount;
			if (m_GpuHistory.size() == MaximumGpuTimingSamples)
			{
				m_GpuHistory.erase(m_GpuHistory.begin());
				++Target.Snapshot.GpuTimingCaptureHistoryEvictionCount;
			}
			m_GpuHistory.push_back(std::move(Sample));
		}
		Target.Snapshot.CompletedGpuTimings = m_GpuHistory;
	}

	void EditorPerformanceCaptureService::AppendResourceSegment(
		const EditorPerformanceSnapshot& Snapshot, const EditorPerformanceCaptureMetadata& Metadata)
	{
		EditorPerformanceResourceSegment Segment;
		Segment.TimestampNanoseconds = Snapshot.Diagnostics.SnapshotTimestampNanoseconds;
		Segment.Workload = Metadata.Workload;
		Segment.ContentFingerprint = Metadata.ContentFingerprint;
		Segment.ContentFingerprintScope = Metadata.ContentFingerprintScope;
		Segment.ContentFingerprintUnavailableReason = Metadata.ContentFingerprintUnavailableReason;
		const size_t MetricCount = (std::min)(Snapshot.Resources.size(), MaximumMetricsPerResourceSegment);
		Segment.Resources.assign(Snapshot.Resources.begin(), Snapshot.Resources.begin() + static_cast<std::ptrdiff_t>(MetricCount));
		m_ActiveCapture->Snapshot.ResourceSegmentMetricTruncationCount += Snapshot.Resources.size() - MetricCount;

		if (!m_ActiveCapture->ResourceSegments.empty())
		{
			const EditorPerformanceResourceSegment& Previous = m_ActiveCapture->ResourceSegments.back();
			if (Previous.Workload == Segment.Workload && Previous.ContentFingerprint == Segment.ContentFingerprint &&
				Previous.ContentFingerprintScope == Segment.ContentFingerprintScope &&
				Previous.ContentFingerprintUnavailableReason == Segment.ContentFingerprintUnavailableReason &&
				MetricsEqual(Previous.Resources, Segment.Resources))
				return;
		}
		if (m_ActiveCapture->ResourceSegments.size() == MaximumResourceSegments)
		{
			m_ActiveCapture->ResourceSegments.erase(m_ActiveCapture->ResourceSegments.begin());
			++m_ActiveCapture->Snapshot.ResourceSegmentHistoryEvictionCount;
		}
		m_ActiveCapture->ResourceSegments.push_back(std::move(Segment));
	}

	void EditorPerformanceCaptureService::UpdateGpuStatus(EditorPerformanceSnapshot& Snapshot, bool Completed) const
	{
		Snapshot.GpuTimingAvailable = false;
		Snapshot.GpuTimingAvailable = Snapshot.GpuTimingPositiveDurationCount != 0;
		const bool HasZeroDurationSample = Snapshot.GpuTimingZeroDurationCount != 0;
		if (Snapshot.GpuTimingFailureReported)
			Snapshot.GpuTimingResultStatus = EditorGpuTimingResultStatus::Failed;
		else if (Snapshot.GpuTimingAvailable)
			Snapshot.GpuTimingResultStatus = EditorGpuTimingResultStatus::Informative;
		else if (HasZeroDurationSample)
			Snapshot.GpuTimingResultStatus = EditorGpuTimingResultStatus::ZeroDurationOnly;
		else if (!Completed && Snapshot.GpuTimingEnabled)
			Snapshot.GpuTimingResultStatus = EditorGpuTimingResultStatus::Pending;
		else
			Snapshot.GpuTimingResultStatus = EditorGpuTimingResultStatus::NoSamples;

		if (Snapshot.GpuTimingResultStatus == EditorGpuTimingResultStatus::Informative)
			Snapshot.GpuTimingUnavailableReason.clear();
		else if (Snapshot.GpuTimingResultStatus == EditorGpuTimingResultStatus::ZeroDurationOnly)
			Snapshot.GpuTimingUnavailableReason =
				"GPU queries returned only zero-duration samples; they do not establish useful frame-duration evidence.";
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
				{ "engine_build_identifier_unavailable_reason", Capture.Metadata.BuildIdentifier.empty() &&
					Capture.Metadata.BuildIdentifierUnavailableReason.empty()
					? "Not supplied by the capture producer" : Capture.Metadata.BuildIdentifierUnavailableReason },
				{ "hardware", Capture.Metadata.Hardware.empty() ? "unavailable" : Capture.Metadata.Hardware },
				{ "hardware_unavailable_reason", Capture.Metadata.Hardware.empty() && Capture.Metadata.HardwareUnavailableReason.empty()
					? "Not supplied by the capture producer" : Capture.Metadata.HardwareUnavailableReason },
				{ "content_fingerprint", Capture.Metadata.ContentFingerprint },
				{ "content_fingerprint_scope", Capture.Metadata.ContentFingerprintScope },
				{ "content_fingerprint_unavailable_reason", Capture.Metadata.ContentFingerprint.empty() &&
					Capture.Metadata.ContentFingerprintUnavailableReason.empty()
					? "Not supplied by the capture producer" : Capture.Metadata.ContentFingerprintUnavailableReason },
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
				{ "diagnostic_events_exported", (std::min)(Diagnostics.Events.size(), PulseForge::DiagnosticRecorder::MaximumCapacity) },
				{ "diagnostic_snapshot_truncation_count", Snapshot.DiagnosticSnapshotTruncationCount },
				{ "gpu_timing_supported", Snapshot.GpuTimingSupported },
				{ "gpu_timing_requested", Snapshot.GpuTimingRequested },
				{ "gpu_timing_enabled", Snapshot.GpuTimingEnabled },
				{ "gpu_timing_available", Snapshot.GpuTimingAvailable },
				{ "gpu_timing_failure_reported", Snapshot.GpuTimingFailureReported },
				{ "gpu_timing_result_status", GpuResultStatusName(Snapshot.GpuTimingResultStatus) },
				{ "gpu_timing_unavailable_reason", Snapshot.GpuTimingUnavailableReason },
				{ "gpu_timing_renderer_overwrite_count", Snapshot.GpuTimingRendererOverwriteCount },
				{ "gpu_timing_capture_history_eviction_count", Snapshot.GpuTimingCaptureHistoryEvictionCount },
				{ "gpu_timing_stale_session_discard_count", Snapshot.GpuTimingStaleSessionDiscardCount },
				{ "gpu_timing_invalid_result_count", Snapshot.GpuTimingInvalidResultCount },
				{ "gpu_timing_positive_duration_count", Snapshot.GpuTimingPositiveDurationCount },
				{ "gpu_timing_zero_duration_count", Snapshot.GpuTimingZeroDurationCount },
				{ "gpu_timing_export_truncation_count", Snapshot.GpuTimingExportTruncationCount },
				{ "resource_segment_history_eviction_count", Snapshot.ResourceSegmentHistoryEvictionCount },
				{ "resource_segment_metric_truncation_count", Snapshot.ResourceSegmentMetricTruncationCount }
			};
			Json& Metadata = Document["metadata"];
			Metadata["resources"] = Json::array();
			Metadata["resource_metrics_in_snapshot"] = Snapshot.Resources.size();
			const size_t ResourceCount = (std::min)(Snapshot.Resources.size(), MaximumMetricsPerResourceSegment);
			Metadata["resource_metrics_exported"] = ResourceCount;
			for (size_t Index = 0; Index < ResourceCount; ++Index)
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
			const size_t GpuSampleCount = (std::min)(Snapshot.CompletedGpuTimings.size(), MaximumGpuTimingSamples);
			Metadata["gpu_timing_samples_in_snapshot"] = Snapshot.CompletedGpuTimings.size();
			Metadata["gpu_timing_samples_exported"] = GpuSampleCount;
			const size_t ExportTruncation = Snapshot.CompletedGpuTimings.size() - GpuSampleCount;
			Metadata["gpu_timing_export_truncation_count"] = Snapshot.GpuTimingExportTruncationCount + ExportTruncation;
			for (size_t Index = 0; Index < GpuSampleCount; ++Index)
			{
				const EditorGpuTimingSample& Sample = Snapshot.CompletedGpuTimings[Index];
				Metadata["gpu_timings"].push_back({
					{ "session_id", Sample.SessionId },
					{ "frame_sequence", Sample.FrameSequence },
					{ "duration_ns", Sample.DurationNanoseconds },
					{ "quality", Sample.Quality == EditorGpuTimingSampleQuality::PositiveDuration ? "positive-duration" : "zero-duration" },
					{ "collected_at_monotonic_ns", Sample.TimestampNanoseconds },
					{ "name", Sample.Name },
					{ "owner", Sample.Owner },
					{ "source", Sample.Source }
				});
			}
			Metadata["resource_segments"] = Json::array();
			const size_t SegmentCount = (std::min)(Capture.ResourceSegments.size(), MaximumResourceSegments);
			Metadata["resource_segments_in_snapshot"] = Capture.ResourceSegments.size();
			Metadata["resource_segments_exported"] = SegmentCount;
			for (size_t SegmentIndex = 0; SegmentIndex < SegmentCount; ++SegmentIndex)
			{
				const EditorPerformanceResourceSegment& Segment = Capture.ResourceSegments[SegmentIndex];
				Json Entry = {
					{ "timestamp_monotonic_ns", Segment.TimestampNanoseconds },
					{ "workload", Segment.Workload },
					{ "content_fingerprint", Segment.ContentFingerprint },
					{ "content_fingerprint_scope", Segment.ContentFingerprintScope },
					{ "content_fingerprint_unavailable_reason", Segment.ContentFingerprintUnavailableReason },
					{ "resources", Json::array() }
				};
				const size_t SegmentMetricCount = (std::min)(Segment.Resources.size(), MaximumMetricsPerResourceSegment);
				for (size_t MetricIndex = 0; MetricIndex < SegmentMetricCount; ++MetricIndex)
				{
					const EditorResourceMetric& Metric = Segment.Resources[MetricIndex];
					Entry["resources"].push_back({
						{ "label", Metric.Label }, { "owner", Metric.Owner }, { "scope", Metric.Scope },
						{ "value", Metric.Value },
						{ "unit", Metric.Kind == EditorResourceMetricKind::Bytes ? "bytes" : "count" },
						{ "quality", Metric.Quality == EditorResourceMetricQuality::ExactPayload ? "exact-payload" :
							Metric.Quality == EditorResourceMetricQuality::LogicalAllocation ? "logical-allocation-estimate" :
							Metric.Quality == EditorResourceMetricQuality::ProcessReported ? "os-reported" : "unavailable" },
						{ "quality_reason", Metric.QualityReason },
						{ "unavailable_reason", Metric.AvailabilityReason }
					});
				}
				Metadata["resource_segments"].push_back(std::move(Entry));
			}

			Document["traceEvents"] = Json::array();
			const size_t EventCount = (std::min)(Diagnostics.Events.size(), PulseForge::DiagnosticRecorder::MaximumCapacity);
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
			return std::unexpected(MakeExportError(EditorPerformanceExportErrorCode::SerializeFailed, {}, Error.what()));
		}
	}

	std::expected<void, EditorPerformanceExportError> EditorPerformanceCaptureService::ExportAtomically(
		const std::filesystem::path& Destination, const EditorPerformanceCapture& Capture)
	{
		if (Destination.empty())
			return std::unexpected(MakeExportError(EditorPerformanceExportErrorCode::InvalidCapture, Destination,
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
					return std::unexpected(MakeExportError(EditorPerformanceExportErrorCode::TemporaryWriteFailed, TemporaryPath,
						"Could not create the temporary performance capture file"));
				}
				Output.write(Contents->data(), static_cast<std::streamsize>(Contents->size()));
				Output.flush();
				Output.close();
				if (!Output)
				{
					RemoveTemporaryFile(TemporaryPath);
					return std::unexpected(MakeExportError(EditorPerformanceExportErrorCode::TemporaryWriteFailed, TemporaryPath,
						"Could not write the temporary performance capture file"));
				}
			}
		}
		catch (const std::exception& Error)
		{
			if (!TemporaryPath.empty()) RemoveTemporaryFile(TemporaryPath);
			return std::unexpected(MakeExportError(EditorPerformanceExportErrorCode::TemporaryWriteFailed,
				TemporaryPath.empty() ? Destination : TemporaryPath, Error.what()));
		}

		if (auto Replaced = EditorPlatformFileSystem::ReplaceFileAtomically(TemporaryPath, Destination); !Replaced)
		{
			RemoveTemporaryFile(TemporaryPath);
			return std::unexpected(MakeExportError(EditorPerformanceExportErrorCode::AtomicReplaceFailed, Destination,
				std::move(Replaced.error())));
		}
		return {};
	}
} // namespace PulseForgeEditor
