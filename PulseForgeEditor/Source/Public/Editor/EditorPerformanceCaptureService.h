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

	// Raw renderer data. Counters are monotonic for one RendererGeneration; samples are newly drained only.
	struct RendererTimingLifetimeCounters
	{
		uint64_t RendererGeneration = 1;
		uint64_t QueueOverwriteCount = 0;
		uint64_t InvalidResultCount = 0;
	};

	struct RendererTimingObservation
	{
		uint64_t CaptureSessionId = 0;
		RendererTimingLifetimeCounters LifetimeCounters;
		std::vector<EditorGpuTimingSample> NewlyCompletedSamples;
		bool Supported = false;
		bool Requested = false;
		bool Enabled = false;
		bool FailureReported = false;
		bool CollectionOutstanding = false;
		std::string UnavailableReason;
	};

	// All counters in this type are capture-relative and are never accepted as renderer input.
	struct CaptureTimingStatistics
	{
		uint64_t RendererQueueOverwriteCount = 0;
		uint64_t CaptureHistoryEvictionCount = 0;
		uint64_t StaleSessionDiscardCount = 0;
		uint64_t InvalidResultCount = 0;
		uint64_t CounterRegressionCount = 0;
		uint64_t RendererGenerationChangeCount = 0;
		uint64_t PositiveDurationCount = 0;
		uint64_t ZeroDurationCount = 0;
		uint64_t ExportTruncationCount = 0;
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
		CaptureTimingStatistics Timing;
		EditorGpuTimingResultStatus GpuTimingResultStatus = EditorGpuTimingResultStatus::NoSamples;
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
			const PulseForge::DiagnosticSnapshot& InitialDiagnostics,
			const RendererTimingObservation& InitialRendererObservation,
			const std::vector<EditorResourceMetric>& InitialResources,
			EditorPerformanceCaptureMetadata Metadata);
		// Renderer observations always carry lifetime counters and only newly drained samples.
		// After Stop, updates append delayed GPU results and raw counter deltas only.
		[[nodiscard]] std::expected<void, EditorPerformanceCaptureError> Observe(
			const RendererTimingObservation& RendererObservation,
			uint64_t ObservationTimestampNanoseconds,
			const std::vector<EditorResourceMetric>* Resources = nullptr,
			const EditorPerformanceCaptureMetadata* Metadata = nullptr);
		[[nodiscard]] std::expected<void, EditorPerformanceCaptureError> Stop(
			const PulseForge::DiagnosticSnapshot& FinalDiagnostics,
			const RendererTimingObservation& FinalRendererObservation,
			const std::vector<EditorResourceMetric>& FinalResources,
			EditorPerformanceCaptureMetadata FinalMetadata);
		[[nodiscard]] std::optional<EditorPerformanceCapture> GetExportSnapshot(
			const PulseForge::DiagnosticSnapshot* ActiveDiagnostics = nullptr) const;
		[[nodiscard]] const EditorPerformanceSnapshot* GetPresentationSnapshot(
			const PulseForge::DiagnosticSnapshot* ActiveDiagnostics = nullptr);
		[[nodiscard]] bool IsCollectionSettled() const noexcept;
		[[nodiscard]] bool NeedsLifecycleUpdate() const noexcept { return IsActive() || !IsCollectionSettled(); }
		[[nodiscard]] bool IsActive() const noexcept { return m_ActiveCapture.has_value(); }
		[[nodiscard]] bool HasCapture() const noexcept { return m_ActiveCapture.has_value() || m_CompletedCapture.has_value(); }
		[[nodiscard]] uint64_t GetSessionId() const noexcept;
		[[nodiscard]] uint64_t GetFullSnapshotMaterializationCount() const noexcept { return m_FullSnapshotMaterializationCount; }

		[[nodiscard]] static std::expected<std::string, EditorPerformanceExportError> Serialize(
			const EditorPerformanceCapture& Capture);

		[[nodiscard]] static std::expected<void, EditorPerformanceExportError> ExportAtomically(
			const std::filesystem::path& Destination, const EditorPerformanceCapture& Capture);

	private:
		void AppendGpuSamples(EditorPerformanceCapture& Target, const RendererTimingObservation& Observation);
		void AppendResourceSegment(
			EditorPerformanceCapture& Target,
			uint64_t TimestampNanoseconds,
			const std::vector<EditorResourceMetric>& Resources,
			const EditorPerformanceCaptureMetadata& Metadata);
		void ApplyLifetimeCounterDeltas(EditorPerformanceCapture& Target, const RendererTimingObservation& Observation);
		void UpdateGpuStatus(EditorPerformanceSnapshot& Snapshot) const;

		std::optional<EditorPerformanceCapture> m_ActiveCapture;
		std::optional<EditorPerformanceCapture> m_CompletedCapture;
		RendererTimingLifetimeCounters m_LastLifetimeCounters;
		bool m_CollectionOutstanding = false;
		uint64_t m_FullSnapshotMaterializationCount = 0;
	};
} // namespace PulseForgeEditor
