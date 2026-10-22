#pragma once

#include "Core/Diagnostics.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace PulseForgeEditor
{
	class EditorStyle;

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
		ProcessReported
	};

	struct EditorResourceMetric
	{
		std::string Label;
		std::string Owner;
		std::string Scope;
		uint64_t Value = 0;
		EditorResourceMetricKind Kind = EditorResourceMetricKind::Count;
		EditorResourceMetricQuality Quality = EditorResourceMetricQuality::ExactPayload;
	};

	// Contains only completed query results; the panel never polls or waits for GPU work.
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
		std::vector<EditorResourceMetric> Resources;
		std::vector<EditorGpuTimingSample> CompletedGpuTimings;
	};

	struct EditorPerformanceTraceMetadata
	{
		std::string Platform;
		std::string Configuration;
		std::string Renderer;
	};

	[[nodiscard]] std::string SerializeEditorPerformanceTrace(
		const EditorPerformanceSnapshot& Snapshot, const EditorPerformanceTraceMetadata& Metadata);

	enum class EditorPerformanceActionKind : uint8_t
	{
		None,
		StartCapture,
		StopCapture,
		ExportCapture
	};

	struct EditorPerformanceAction
	{
		EditorPerformanceActionKind Kind = EditorPerformanceActionKind::None;
	};

	class EditorPerformancePanel final
	{
	public:
		static constexpr size_t MaximumResourceMetrics = 32;
		static constexpr size_t MaximumVisibleEvents = 256;
		static constexpr size_t MaximumVisibleGpuTimings = 64;

		// Draws a read-only view. The caller handles capture lifecycle and export actions.
		[[nodiscard]] EditorPerformanceAction Draw(const EditorPerformanceSnapshot& Snapshot, EditorStyle& Style) const;
	};
} // namespace PulseForgeEditor
