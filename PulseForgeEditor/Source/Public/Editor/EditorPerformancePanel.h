#pragma once

#include "Editor/EditorPerformanceCaptureService.h"
#include "Editor/EditorResourceInventory.h"

#include <cstddef>
#include <cstdint>
#include <array>

namespace PulseForge
{
	struct SceneRendererResourceCacheStats;
}

namespace PulseForgeEditor
{
	class EditorStyle;

	enum class EditorPerformanceActionKind : uint8_t
	{
		None,
		StartCapture,
		StopCapture,
		ExportCapture,
		RefreshResourceInventory
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
		static constexpr size_t MaximumVisibleInventoryEntries = 256;

		// Draws panel contents inside a utility window owned and classified by the editor layout.
		[[nodiscard]] EditorPerformanceAction DrawContents(
			const EditorPerformanceSnapshot& Snapshot,
			const EditorResourceInventorySnapshot* Inventory,
			const PulseForge::SceneRendererResourceCacheStats* RendererResources,
			EditorStyle& Style) const;

	private:
		mutable std::array<char, 128> m_ResourceInventoryFilter{};
	};
} // namespace PulseForgeEditor
