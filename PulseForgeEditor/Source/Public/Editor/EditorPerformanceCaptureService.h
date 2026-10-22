#pragma once

#include "Core/Diagnostics.h"

#include <cstdint>
#include <expected>
#include <filesystem>
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

	// Contains only completed query results; presentation never polls or waits for GPU work.
	struct EditorGpuTimingSample
	{
		std::string Name;
		std::string Owner;
		std::string Source;
		uint64_t FrameSequence = 0;
		uint64_t TimestampNanoseconds = 0;
		uint64_t DurationNanoseconds = 0;
	};

	struct EditorPerformanceSnapshot
	{
		PulseForge::DiagnosticSnapshot Diagnostics;
		bool GpuTimingAvailable = false;
		bool GpuTimingSupported = false;
		bool GpuTimingEnabled = false;
		std::string GpuTimingUnavailableReason;
		uint64_t GpuTimingDroppedCount = 0;
		uint64_t GpuTimingStaleSessionDiscardCount = 0;
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
		std::string ContentFingerprintUnavailableReason;
	};

	struct EditorPerformanceCapture
	{
		EditorPerformanceSnapshot Snapshot;
		EditorPerformanceCaptureMetadata Metadata;
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

	// Owns stable capture copies and trace serialization/export. It has no application, UI, or dialog state.
	class EditorPerformanceCaptureService final
	{
	public:
		[[nodiscard]] static bool IsGpuTimingAvailable(
			bool Supported, bool EnabledForCapture, bool CaptureActive, bool BackendEnabled, bool FailureReported) noexcept;

		[[nodiscard]] static EditorPerformanceCapture Capture(
			const EditorPerformanceSnapshot& Snapshot, EditorPerformanceCaptureMetadata Metadata);

		[[nodiscard]] static std::expected<std::string, EditorPerformanceExportError> Serialize(
			const EditorPerformanceCapture& Capture);

		[[nodiscard]] static std::expected<void, EditorPerformanceExportError> ExportAtomically(
			const std::filesystem::path& Destination, const EditorPerformanceCapture& Capture);
	};
} // namespace PulseForgeEditor
