#pragma once

#include "Core/Diagnostics.h"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace PulseForgeEditor
{
	enum class EditorResourceMetricKind : uint8_t
	{
		Count,
		Bytes
	};

	// Describes what the reported number measures. These values must not be added into a total.
	enum class EditorResourceMetricQuality : uint8_t
	{
		ExactPayload,
		LogicalAllocation,
		ProcessReported,
		Unavailable
	};

	struct EditorResourceMetric
	{
		std::string Label;
		std::string Owner;
		std::string Scope;
		uint64_t Value = 0;
		EditorResourceMetricKind Kind = EditorResourceMetricKind::Count;
		EditorResourceMetricQuality Quality = EditorResourceMetricQuality::ExactPayload;
		std::string AvailabilityReason;
		std::string QualityReason;
	};

	enum class EditorGpuTimingSampleQuality : uint8_t
	{
		PositiveDuration,
		ZeroDuration
	};

	// Contains completed query results; Observe receives only newly drained results, never polls the renderer.
	struct EditorGpuTimingSample
	{
		uint64_t SessionId = 0;
		std::string Name;
		std::string Owner;
		std::string Source;
		uint64_t FrameSequence = 0;
		uint64_t TimestampNanoseconds = 0;
		uint64_t DurationNanoseconds = 0;
		EditorGpuTimingSampleQuality Quality = EditorGpuTimingSampleQuality::PositiveDuration;
	};

	enum class EditorGpuTimingResultStatus : uint8_t
	{
		Pending,
		Informative,
		ZeroDurationOnly,
		NoSamples,
		Failed
	};

	struct EditorPerformanceSnapshot
	{
		PulseForge::DiagnosticSnapshot Diagnostics;
		bool GpuTimingAvailable = false;
		bool GpuTimingSupported = false;
		bool GpuTimingRequested = false;
		bool GpuTimingEnabled = false;
		std::string GpuTimingUnavailableReason;
		bool GpuTimingFailureReported = false;
		// Input is renderer-cumulative; service output is the delta observed in this capture.
		uint64_t GpuTimingRendererOverwriteCount = 0;
		// Input is renderer-cumulative; service output is the delta observed in this capture.
		uint64_t GpuTimingInvalidResultCount = 0;
		EditorGpuTimingResultStatus GpuTimingResultStatus = EditorGpuTimingResultStatus::NoSamples;
		uint64_t GpuTimingCaptureHistoryEvictionCount = 0;
		uint64_t GpuTimingStaleSessionDiscardCount = 0;
		uint64_t GpuTimingPositiveDurationCount = 0;
		uint64_t GpuTimingZeroDurationCount = 0;
		uint64_t GpuTimingExportTruncationCount = 0;
		uint64_t DiagnosticSnapshotTruncationCount = 0;
		uint64_t ResourceSegmentHistoryEvictionCount = 0;
		uint64_t ResourceSegmentMetricTruncationCount = 0;
		std::vector<EditorResourceMetric> Resources;
		std::vector<EditorGpuTimingSample> CompletedGpuTimings;
	};

	struct EditorPerformanceCaptureMetadata
	{
		std::string Platform;
		std::string Configuration;
		std::string Renderer;
		std::string PlatformUnavailableReason;
		std::string ConfigurationUnavailableReason;
		std::string RendererUnavailableReason;
		std::string Workload;
		std::string BuildIdentifier;
		std::string BuildIdentifierUnavailableReason;
		std::string Hardware;
		std::string HardwareUnavailableReason;
		std::string ContentFingerprint;
		std::string ContentFingerprintScope = "selected project and scene files";
		std::string ContentFingerprintUnavailableReason;
	};

	struct EditorPerformanceResourceSegment
	{
		uint64_t TimestampNanoseconds = 0;
		std::string Workload;
		std::string ContentFingerprint;
		std::string ContentFingerprintScope = "selected project and scene files";
		std::string ContentFingerprintUnavailableReason;
		std::vector<EditorResourceMetric> Resources;
	};

	struct EditorPerformanceCapture
	{
		EditorPerformanceSnapshot Snapshot;
		EditorPerformanceCaptureMetadata Metadata;
		std::vector<EditorPerformanceResourceSegment> ResourceSegments;
	};

	enum class EditorPerformanceCaptureErrorCode : uint8_t
	{
		AlreadyActive,
		NotActive,
		StaleSession
	};

	struct EditorPerformanceCaptureError
	{
		EditorPerformanceCaptureErrorCode Code = EditorPerformanceCaptureErrorCode::NotActive;
		std::string Message;
	};

	enum class EditorPerformanceExportErrorCode : uint8_t
	{
		InvalidCapture,
		SerializeFailed,
		TemporaryWriteFailed,
		AtomicReplaceFailed
	};

	struct EditorPerformanceExportError
	{
		EditorPerformanceExportErrorCode Code = EditorPerformanceExportErrorCode::SerializeFailed;
		std::filesystem::path Path;
		std::string Message;
	};

	// Owns one bounded active and one completed capture. It has no application, UI, or dialog state.
	class EditorPerformanceCaptureService final
	{
	public:
		static constexpr size_t MaximumGpuTimingSamples = 64;
		static constexpr size_t MaximumResourceSegments = 64;
		static constexpr size_t MaximumMetricsPerResourceSegment = 32;

		[[nodiscard]] std::expected<void, EditorPerformanceCaptureError> Begin(
			const EditorPerformanceSnapshot& InitialSnapshot, EditorPerformanceCaptureMetadata Metadata);
		// CompletedGpuTimings contains only samples newly drained since the prior service call.
		// After Stop, same-session observations append delayed GPU results only; frozen session data stays unchanged.
		[[nodiscard]] std::expected<void, EditorPerformanceCaptureError> Observe(
			const EditorPerformanceSnapshot& Snapshot, const EditorPerformanceCaptureMetadata& Metadata);
		[[nodiscard]] std::expected<void, EditorPerformanceCaptureError> Stop(
			const EditorPerformanceSnapshot& FinalSnapshot, EditorPerformanceCaptureMetadata FinalMetadata);
		[[nodiscard]] std::optional<EditorPerformanceCapture> GetExportSnapshot() const;
		[[nodiscard]] std::optional<EditorPerformanceSnapshot> GetCurrentSnapshot() const;
		[[nodiscard]] bool IsActive() const noexcept { return m_ActiveCapture.has_value(); }
		[[nodiscard]] bool HasCapture() const noexcept { return m_ActiveCapture.has_value() || m_CompletedCapture.has_value(); }
		[[nodiscard]] uint64_t GetSessionId() const noexcept;
		void ApplyGpuHistory(EditorPerformanceSnapshot& Snapshot) const;

		[[nodiscard]] static std::expected<std::string, EditorPerformanceExportError> Serialize(
			const EditorPerformanceCapture& Capture);

		[[nodiscard]] static std::expected<void, EditorPerformanceExportError> ExportAtomically(
			const std::filesystem::path& Destination, const EditorPerformanceCapture& Capture);

	private:
		void AppendGpuSamples(EditorPerformanceCapture& Target, const EditorPerformanceSnapshot& Snapshot);
		void AppendResourceSegment(const EditorPerformanceSnapshot& Snapshot, const EditorPerformanceCaptureMetadata& Metadata);
		void UpdateGpuStatus(EditorPerformanceSnapshot& Snapshot, bool Completed) const;

		std::optional<EditorPerformanceCapture> m_ActiveCapture;
		std::optional<EditorPerformanceCapture> m_CompletedCapture;
		std::vector<EditorGpuTimingSample> m_GpuHistory;
		uint64_t m_LastRendererOverwriteCount = 0;
		uint64_t m_LastInvalidResultCount = 0;
		uint64_t m_LastGpuFrameSequence = 0;
		bool m_HasLastGpuFrameSequence = false;
	};
} // namespace PulseForgeEditor
