#pragma once

#include <cmath>
#include <cstdint>
#include <expected>
#include <limits>
#include <string>

namespace PulseForgeEditor
{
	struct EditorViewportTargetExtent
	{
		uint32_t Width = 0;
		uint32_t Height = 0;
	};

	struct EditorViewportSceneMode
	{
		bool UseRuntimeScene = false;
		bool ViewProjectionValid = false;
	};

	[[nodiscard]] constexpr EditorViewportSceneMode GetEditorViewportSceneMode(
		bool RuntimeActive, bool HasEditScene, bool HasRuntimeScene) noexcept
	{
		return { RuntimeActive && HasRuntimeScene, !RuntimeActive && HasEditScene };
	}

	[[nodiscard]] inline std::expected<EditorViewportTargetExtent, std::string> CalculateEditorViewportTargetExtent(
		double LogicalWidth, double LogicalHeight, double ScaleX, double ScaleY)
	{
		const double WidthInPixels = std::ceil(LogicalWidth * (ScaleX > 0.0 ? ScaleX : 1.0));
		const double HeightInPixels = std::ceil(LogicalHeight * (ScaleY > 0.0 ? ScaleY : 1.0));
		constexpr uint32_t ResolutionQuantum = 32;
		if (!std::isfinite(WidthInPixels) || !std::isfinite(HeightInPixels) || WidthInPixels < 1.0 || HeightInPixels < 1.0 ||
			WidthInPixels > (std::numeric_limits<uint32_t>::max)() - ResolutionQuantum ||
			HeightInPixels > (std::numeric_limits<uint32_t>::max)() - ResolutionQuantum)
			return std::unexpected("Viewport dimensions are outside the supported range");

		const auto Quantize = [](double Dimension)
		{ return static_cast<uint32_t>(std::ceil(Dimension / ResolutionQuantum) * ResolutionQuantum); };
		return EditorViewportTargetExtent{ Quantize(WidthInPixels), Quantize(HeightInPixels) };
	}
} // namespace PulseForgeEditor
