#pragma once

#include "Editor/EditorPerformanceCaptureService.h"

#include <cstddef>
#include <cstdint>

namespace PulseForgeEditor
{
	class EditorStyle;

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

		// Draws panel contents inside a utility window owned and classified by the editor layout.
		[[nodiscard]] EditorPerformanceAction DrawContents(const EditorPerformanceSnapshot& Snapshot, EditorStyle& Style) const;
	};
} // namespace PulseForgeEditor
